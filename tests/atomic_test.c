/*
 * Atomic primitive test summary:
 * - Verifies spin_lock_init leaves the lock unlocked.
 * - Verifies spin_lock_try_acquire/spin_lock_acquire disable interrupts and
 *   spin_lock_release restores the prior IMR state.
 * - Verifies contended spin lock users preserve mutual exclusion.
 * - Verifies spin_barrier_sync does not release any participant early.
 */

#include "../kernel/atomic.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

#define LOCK_WORKERS 6
#define LOCK_ROUNDS 64
#define LOCK_PAUSE_ITERS 16
#define BARRIER_THREADS 4
#define IMR_GLOBAL_ENABLE 0x80000000

static struct SpinLock test_lock;

static int critical_inside = 0;
static int shared_counter = 0;
static int lock_workers_done = 0;

static int barrier_counter = 0;
static int barrier_arrived = 0;
static int barrier_passed = 0;

static void fail_int(char* msg, int got, int expected) {
  int args[2] = { got, expected };
  say("***atomic FAIL got=%d expected=%d\n", args);
  panic(msg);
}

static void fail_hex(char* msg, unsigned got, unsigned expected) {
  int args[2] = { (int)got, (int)expected };
  say("***atomic FAIL got=0x%X expected=0x%X\n", args);
  panic(msg);
}

static void lock_worker(void* arg) {
  (void)arg;

  for (int i = 0; i < LOCK_ROUNDS; i++) {
    spin_lock_acquire(&test_lock);

    int prior_inside = __atomic_fetch_add(&critical_inside, 1);
    if (prior_inside != 0) {
      fail_int("atomic test: concurrent spin lock entry detected\n", prior_inside, 0);
    }

    __atomic_fetch_add(&shared_counter, 1);

    for (int spin = 0; spin < LOCK_PAUSE_ITERS; spin++) {
      pause();
    }

    int prior_leave = __atomic_fetch_add(&critical_inside, -1);
    if (prior_leave != 1) {
      fail_int("atomic test: spin lock exit count corrupted\n", prior_leave, 1);
    }

    spin_lock_release(&test_lock);
    yield();
  }

  __atomic_fetch_add(&lock_workers_done, 1);
}

static void barrier_worker(void* arg) {
  (void)arg;

  __atomic_fetch_add(&barrier_arrived, 1);
  spin_barrier_sync(&barrier_counter);

  int arrived = __atomic_load_n(&barrier_arrived);
  if (arrived != BARRIER_THREADS) {
    fail_int("atomic test: spin barrier released early\n", arrived, BARRIER_THREADS);
  }

  __atomic_fetch_add(&barrier_passed, 1);
}

static void spawn_worker(void (*func)(void*)) {
  struct Fun* fun = malloc(sizeof(struct Fun));
  assert(fun != NULL, "atomic test: Fun allocation failed.\n");
  fun->func = func;
  fun->arg = NULL;
  thread(fun);
}

void kernel_main(void) {
  say("***atomic test start\n", NULL);

  spin_lock_init(&test_lock);
  if (test_lock.the_lock != 0) {
    fail_int("atomic test: spin_lock_init did not clear the lock\n", test_lock.the_lock, 0);
  }
  if (test_lock.interrupt_state != 0) {
    fail_int("atomic test: spin_lock_init did not clear interrupt_state\n",
             test_lock.interrupt_state, 0);
  }

  unsigned imr_before = get_imr();
  if ((imr_before & IMR_GLOBAL_ENABLE) == 0) {
    fail_hex("atomic test: kernel_main started with interrupts disabled\n", imr_before,
             IMR_GLOBAL_ENABLE);
  }

  if (!spin_lock_try_acquire(&test_lock)) {
    panic("atomic test: spin_lock_try_acquire failed on an unlocked lock\n");
  }

  unsigned imr_during_try = get_imr();
  if ((imr_during_try & IMR_GLOBAL_ENABLE) != 0) {
    fail_hex("atomic test: try_acquire left interrupts enabled\n", imr_during_try, 0);
  }

  if (spin_lock_try_acquire(&test_lock)) {
    panic("atomic test: nested spin_lock_try_acquire unexpectedly succeeded\n");
  }

  unsigned imr_after_failed_try = get_imr();
  if ((imr_after_failed_try & IMR_GLOBAL_ENABLE) != 0) {
    fail_hex("atomic test: failed try_acquire restored interrupts too early\n",
             imr_after_failed_try, 0);
  }

  spin_lock_release(&test_lock);
  if (get_imr() != imr_before) {
    fail_hex("atomic test: release did not restore IMR after try_acquire\n", get_imr(), imr_before);
  }

  spin_lock_acquire(&test_lock);
  unsigned imr_during_acquire = get_imr();
  if ((imr_during_acquire & IMR_GLOBAL_ENABLE) != 0) {
    fail_hex("atomic test: acquire left interrupts enabled\n", imr_during_acquire, 0);
  }
  spin_lock_release(&test_lock);

  if (get_imr() != imr_before) {
    fail_hex("atomic test: release did not restore IMR after acquire\n", get_imr(), imr_before);
  }

  say("***atomic lock state ok\n", NULL);

  critical_inside = 0;
  shared_counter = 0;
  lock_workers_done = 0;
  for (int i = 0; i < LOCK_WORKERS; i++) {
    spawn_worker(lock_worker);
  }

  while (__atomic_load_n(&lock_workers_done) != LOCK_WORKERS) {
    yield();
  }

  int expected_total = LOCK_WORKERS * LOCK_ROUNDS;
  if (__atomic_load_n(&shared_counter) != expected_total) {
    fail_int("atomic test: spin lock lost counter updates\n", __atomic_load_n(&shared_counter),
             expected_total);
  }
  if (__atomic_load_n(&critical_inside) != 0) {
    fail_int("atomic test: critical section occupancy did not return to zero\n",
             __atomic_load_n(&critical_inside), 0);
  }

  say("***atomic contention ok\n", NULL);

  barrier_counter = BARRIER_THREADS;
  barrier_arrived = 0;
  barrier_passed = 0;
  for (int i = 0; i < BARRIER_THREADS; i++) {
    spawn_worker(barrier_worker);
  }

  while (__atomic_load_n(&barrier_passed) != BARRIER_THREADS) {
    yield();
  }

  if (__atomic_load_n(&barrier_arrived) != BARRIER_THREADS) {
    fail_int("atomic test: not all barrier participants arrived\n",
             __atomic_load_n(&barrier_arrived), BARRIER_THREADS);
  }
  if (__atomic_load_n(&barrier_counter) != 0) {
    fail_int("atomic test: barrier counter did not reach zero\n",
             __atomic_load_n(&barrier_counter), 0);
  }

  say("***atomic barrier ok\n", NULL);
  say("***atomic test complete\n", NULL);
}
