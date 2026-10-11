/*
 * TLB ownership across context switches (vmem_tlb_switch_needs_flush in
 * kernel/vmem.c).
 *
 * A context switch clears the core's TLB only when the entries it holds might
 * be stale for the next thread. These checks pin threads to chosen cores and
 * inspect the TLB with tlb_read(), which looks up (current PID, address)
 * without touching memory, so a stale entry is caught before any access
 * could use it.
 *
 * Validates:
 * 1. Kept: a thread pinned to core 1 yields (switching to the idle thread and
 *    back). Its translation must still be in the TLB, or the optimization is
 *    not happening.
 * 2. munmap after migration: a thread faults a page in on core 1, moves to
 *    core 2, unmaps the page there, maps a fresh page at the same address,
 *    and moves back to core 1. Core 1 must hold no translation for that
 *    address: the old one would still point at the freed frame. This is the
 *    case that makes simply deleting the flush unsafe.
 * 3. Generations: each new address space gets its own nonzero generation,
 *    and munmap moves it on. Fresh generations are what make a page directory
 *    that is freed and reused under the same PID safe.
 * 4. Decision rule: on synthetic TCBs, the TLB is kept for the recorded owner
 *    and for pid-0 threads, and cleared for another pid or for the same pid
 *    with a new generation (a reused page directory).
 *
 * How threads change cores: a thread sets its own core affinity and sleeps;
 * the wakeup goes through scheduler_wake_thread(), which queues a pinned
 * thread on that core (yield() would requeue it locally). Check 1 could be
 * defeated by another thread with a page directory running on core 1 between
 * the yield and the resume: the main thread parks on core 0, and every other
 * kernel thread (idle, reaper, device workers, watchdog) has pid 0.
 */

#include "../kernel/vmem.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"
#include "../kernel/interrupts.h"
#include "../kernel/per_core.h"
#include "../kernel/pit.h"
#include "../kernel/string.h"

#define MAX_WAIT_JIFFIES 3000

static int worker_done;
static unsigned kept_generation;
static unsigned migrate_generation_before;
static unsigned migrate_generation_after;

// Report a failed check with what was seen and stop the kernel.
static void fail(char* check, unsigned got, unsigned expected) {
  int args[3] = {(int)check, (int)got, (int)expected};
  say("***tlb ownership FAIL %s: got 0x%X expected 0x%X\n", args);
  panic("vmem_tlb_ownership: TLB ownership check failed\n");
}

// Move the calling thread to `core` (see the header) and check it got there.
static void move_to_core(unsigned core) {
  unsigned was = interrupts_disable();
  get_current_tcb()->core_affinity = core;
  interrupts_restore(was);
  sleep(1);
  if (get_core_id() != core) fail("move_to_core", get_core_id(), core);
}

// Check 1: a yield on a pinned core keeps the thread's translation.
static void kept_worker(void* arg) {
  (void)arg;
  move_to_core(1);
  kept_generation = get_current_tcb()->tlb_generation;
  unsigned* page = (unsigned*)mmap(FRAME_SIZE, NULL, 0, MMAP_READ | MMAP_WRITE);
  page[0] = 0x1234;
  if (tlb_read(page) == 0) fail("kept: translation after the fault", 0, 1);
  yield();
  if (get_core_id() != 1) fail("kept: core after yield", get_core_id(), 1);
  if (tlb_read(page) == 0) fail("kept: translation after yield", 0, 1);
  munmap(page);
  __atomic_store_n(&worker_done, 1);
}

// Check 2: unmapping on another core retires core 1's translation.
static void migrate_worker(void* arg) {
  (void)arg;
  move_to_core(1);
  unsigned* page = (unsigned*)mmap(FRAME_SIZE, NULL, 0, MMAP_READ | MMAP_WRITE);
  page[0] = 0xA1A1A1A1;
  if (tlb_read(page) == 0) fail("migrate: translation after the fault", 0, 1);

  move_to_core(2);
  migrate_generation_before = get_current_tcb()->tlb_generation;
  munmap(page);
  migrate_generation_after = get_current_tcb()->tlb_generation;
  unsigned* again = (unsigned*)mmap(FRAME_SIZE, NULL, 0, MMAP_READ | MMAP_WRITE);
  if (again != page) fail("migrate: remap address", (unsigned)again, (unsigned)page);
  again[0] = 0xB2B2B2B2;

  move_to_core(1);
  unsigned entry = tlb_read(page);
  if (entry != 0) fail("migrate: core 1 translation after munmap on core 2", entry, 0);
  if (again[0] != 0xB2B2B2B2) fail("migrate: remapped contents", again[0], 0xB2B2B2B2);
  munmap(again);
  __atomic_store_n(&worker_done, 1);
}

// Run `func` on a new thread and wait for it to report completion.
static void run_worker(void (*func)(void*), char* name) {
  __atomic_store_n(&worker_done, 0);
  struct Fun* fun = malloc(sizeof(struct Fun));
  assert(fun != NULL, "vmem_tlb_ownership: Fun allocation failed.\n");
  fun->func = func;
  fun->arg = NULL;
  thread(fun);
  unsigned start = current_jiffies;
  while (__atomic_load_n(&worker_done) == 0) {
    if (current_jiffies - start > MAX_WAIT_JIFFIES) {
      int args[2] = {(int)name, MAX_WAIT_JIFFIES};
      say("***tlb ownership FAIL %s worker did not finish within %d jiffies\n", args);
      panic("vmem_tlb_ownership: worker timed out\n");
    }
    yield();
  }
}

// Check 4: the switch decision on synthetic TCBs. The core's real owner
// record is restored by clearing the TLB and adopting the current thread,
// all with interrupts disabled.
static void decision_checks(void) {
  struct TCB* a = malloc(sizeof(struct TCB));
  struct TCB* reused = malloc(sizeof(struct TCB));
  struct TCB* other = malloc(sizeof(struct TCB));
  struct TCB* no_space = malloc(sizeof(struct TCB));
  assert(a != NULL && reused != NULL && other != NULL && no_space != NULL,
    "vmem_tlb_ownership: TCB allocation failed.\n");
  memset(a, 0, sizeof(struct TCB));
  memset(reused, 0, sizeof(struct TCB));
  memset(other, 0, sizeof(struct TCB));
  memset(no_space, 0, sizeof(struct TCB));
  a->pid = 0x123000;
  a->tlb_generation = 5;
  reused->pid = 0x123000;
  reused->tlb_generation = 6;
  other->pid = 0x456000;
  other->tlb_generation = 6;

  unsigned was = interrupts_disable();
  vmem_tlb_adopt(a);
  bool same_owner = vmem_tlb_switch_needs_flush(a);
  bool pid_zero = vmem_tlb_switch_needs_flush(no_space);
  bool after_pid_zero = vmem_tlb_switch_needs_flush(a);
  bool new_generation = vmem_tlb_switch_needs_flush(reused);
  bool adopted = vmem_tlb_switch_needs_flush(reused);
  bool other_pid = vmem_tlb_switch_needs_flush(other);
  tlb_flush();
  vmem_tlb_adopt(get_current_tcb());
  interrupts_restore(was);

  if (same_owner) fail("decision: same owner", 1, 0);
  if (pid_zero) fail("decision: pid 0", 1, 0);
  if (after_pid_zero) fail("decision: owner after a pid-0 thread", 1, 0);
  if (!new_generation) fail("decision: same pid, new generation", 0, 1);
  if (adopted) fail("decision: new owner again", 1, 0);
  if (!other_pid) fail("decision: another pid", 0, 1);
  free(a);
  free(reused);
  free(other);
  free(no_space);
}

// Run the checks in order; each worker finishes before the next starts. The
// main thread has a page directory too, so it parks itself on core 0, away
// from the cores the workers use.
void kernel_main(void) {
  say("***tlb ownership test start\n", NULL);
  move_to_core(0);

  run_worker(kept_worker, "kept");
  say("***tlb ownership kept ok\n", NULL);

  run_worker(migrate_worker, "migrate");
  say("***tlb ownership munmap after migration ok\n", NULL);

  if (kept_generation == 0) fail("generation: kept worker", 0, 1);
  if (migrate_generation_before == 0) fail("generation: migrate worker", 0, 1);
  if (migrate_generation_before == kept_generation) {
    fail("generation: distinct address spaces", migrate_generation_before, 0);
  }
  if (migrate_generation_after == migrate_generation_before) {
    fail("generation: munmap", migrate_generation_after, 0);
  }
  say("***tlb ownership generations ok\n", NULL);

  decision_checks();
  say("***tlb ownership decisions ok\n", NULL);

  say("***tlb ownership test complete\n", NULL);
}
