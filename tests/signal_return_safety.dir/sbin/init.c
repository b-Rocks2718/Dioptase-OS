/*
 * Asynchronous-signal final-return regression:
 *
 * Behavior under test:
 * - a pending handler is not entered merely because an I/O-blocked TCB became
 *   runnable;
 * - the page-fault continuation first accepts its SD wakeup and releases the
 *   global page-cache BlockingLock;
 * - only the final TLB-exception return then enters the handler.
 *
 * Why this exists:
 * A file-backed user fault holds the page-cache lock while SD I/O blocks. The
 * SD wakeup makes the TCB runnable before the suspended C continuation resumes.
 * Scheduler-side signal delivery can enter this handler at that intermediate
 * point; the handler's second file fault then waits recursively on the same
 * lock and never makes progress.
 *
 * How it works:
 * Before fork, the parent prefaults a shared file-backed phase word. The child
 * stores to that already-resident word immediately before touching an uncached
 * file mapping. The parent observes the store without relying on a syscall
 * wakeup, yields until one jiffy passes for the child to enter the fault, and makes
 * SIGNAL_HELLO pending while deliberately slowed SD DMA is still blocked. The
 * handler faults a distinct uncached mapping. It can complete only if delivery
 * waited until the first page-cache acquisition fully unwound.
 *
 * The parent's wakeup latency is scheduler-dependent, so the signal sometimes
 * lands after the primary fault has finished. That delivery is still correct,
 * so the child waits for its handler without a bound (a lost signal shows up as
 * a test timeout) and reports whether delivery hit the fault window. The
 * parent prints how many iterations exercised the window as an informational
 * "|" line, which is not part of the .ok comparison.
 */

#include "../../../root/crt/stdbool.h"
#include "../../../root/crt/sys.h"
#include "../../../root/crt/atomic.h"
#include "../../user_test.h"

#define STRESS_ITERATIONS 4
#define PRIMARY_FAULT_SETTLE_JIFFIES 1
#define SHARED_PHASE_MAPPING_BYTES 4
#define PRIMARY_FAULT_PHASE 1

// Child exit statuses. Both "delivered" statuses are passes.
#define CHILD_DELIVERED_DURING_FAULT 0
#define CHILD_DELIVERED_AFTER_FAULT 1
#define CHILD_SETUP_FAILED 11
#define CHILD_HANDLER_FAILED 12

static char* handler_mapping = NULL;
static int* shared_phase = NULL;
static int primary_fault_started = 0;
static int primary_fault_finished = 0;
static int handler_completed = 0;
static int delivered_during_fault = 0;

static int page_faulting_handler(int signal){ /* Trigger and record a page fault while returning from a handler. */
  // primary_fault_started is written immediately before the shared phase
  // publication and first mapping access, with no intervening syscall. It
  // distinguishes an unexpected pre-phase delivery from the intended fault.
  if (signal != SIGNAL_HELLO || !primary_fault_started ||
      handler_mapping[0] != 'B'){
    exit(CHILD_HANDLER_FAILED);
  }

  // If the signal arrived during the fault, the handler runs at the TLB
  // exception's final return, before the faulting load re-executes. (An
  // interrupt return between the load and the flag store is counted the same
  // way, so the coverage count is approximate; it is informational only.)
  delivered_during_fault = !primary_fault_finished;
  handler_completed = 1;
  sigreturn(0);
}

static int child_main(void){ /* Install the page-faulting handler and await the parent signal. */
  int primary_fd = open("/primary-data");
  int handler_fd = open("/handler-data");
  if (primary_fd < 0 || handler_fd < 0){
    return CHILD_SETUP_FAILED;
  }

  char* primary_mapping =
    (char*)mmap(4096, primary_fd, 0, MMAP_READ | MMAP_PRIVATE);
  handler_mapping =
    (char*)mmap(4096, handler_fd, 0, MMAP_READ | MMAP_PRIVATE);
  if ((unsigned)primary_mapping == (unsigned)-1 ||
      (unsigned)handler_mapping == (unsigned)-1 ||
      register_handler(SIGNAL_HELLO, (void*)page_faulting_handler) != 0){
    return CHILD_SETUP_FAILED;
  }

  close(primary_fd);
  close(handler_fd);

  /*
   * shared_phase was faulted in before fork, and MAP_SHARED makes this atomic
   * store directly visible to the parent's atomic polling under Dioptase's
   * sequentially-consistent memory model. There is no syscall/final-user-return
   * boundary between this publication and the primary access. The parent adds
   * a one-jiffy settling delay before sending so this child can enter the
   * deliberately slow page fault.
   */
  primary_fault_started = 1;
  __atomic_store_n(shared_phase, PRIMARY_FAULT_PHASE);
  if (primary_mapping[0] != 'A'){
    return CHILD_SETUP_FAILED;
  }
  primary_fault_finished = 1;

  // The parent always sends the signal; each yield's final user return
  // delivers it if it arrived after the fault.
  while (!handler_completed){
    yield();
  }

  return delivered_during_fault ? CHILD_DELIVERED_DURING_FAULT
                                : CHILD_DELIVERED_AFTER_FAULT;
}

static int window_hits = 0;

static int run_once(int iteration){ /* Signal a child during a slow page fault and verify both nested fault returns complete safely. */
  __atomic_store_n(shared_phase, 0);

  int child = fork();
  if (child == 0){
    exit(child_main());
    return -1;
  }

  while (__atomic_load_n(shared_phase) != PRIMARY_FAULT_PHASE){
    yield();
  }

  /*
   * The target-specific SD timing makes one ext2-block read span about two PIT
   * periods. Waiting for the jiffy counter to advance after observing the
   * resident phase store gives the child time to acquire the page-cache lock
   * and block in SD, while the longer transfer supplies margin before DMA
   * completion. Yield rather than sleep(): a sleeping parent is woken only when
   * its core's idle thread next runs the scheduler, which can add several
   * jiffies and let the child's fault finish before the signal is sent.
   */
  unsigned settle_start = get_current_jiffies();
  while (get_current_jiffies() - settle_start < PRIMARY_FAULT_SETTLE_JIFFIES){
    yield();
  }
  int signal_result = signal_child(child, SIGNAL_HELLO);
  int child_result = wait_child(child);
  if (child_result == CHILD_DELIVERED_DURING_FAULT){
    window_hits++;
  }
  bool child_ok = child_result == CHILD_DELIVERED_DURING_FAULT ||
    child_result == CHILD_DELIVERED_AFTER_FAULT;
  int failures = (signal_result != 0) + !child_ok;
  if (failures != 0){
    int args[3] = {iteration, signal_result, child_result};
    printf("| signal return safety: iteration=%d signal_result=%d child_result=%d\n",
      args);
  }
  return failures;
}

int main(void){ /* Verify safe return from signal handlers after faults and nesting. */
  int sync_fd = open("/sync-state");
  if (sync_fd < 0){
    user_test_expect_eq("signals wait for the final kernel-to-user return",
      1, 0);
    return 0;
  }

  shared_phase = (int*)mmap(SHARED_PHASE_MAPPING_BYTES, sync_fd, 0,
    MMAP_READ | MMAP_WRITE | MMAP_SHARED);
  close(sync_fd);
  if ((unsigned)shared_phase == (unsigned)-1){
    user_test_expect_eq("signals wait for the final kernel-to-user return",
      1, 0);
    return 0;
  }

  // Fault the shared word before fork so phase publication itself cannot block.
  __atomic_store_n(shared_phase, 0);

  int failures = 0;
  for (int i = 0; i < STRESS_ITERATIONS; ++i){
    failures += run_once(i);
  }

  int args[2] = {window_hits, STRESS_ITERATIONS};
  printf("| signal return safety: delivered during the fault in %d/%d iterations\n",
    args);
  user_test_expect_eq("signals wait for the final kernel-to-user return",
    failures, 0);
  return 0;
}
