/*
 * Asynchronous-signal final-return regression:
 *
 * Behavior under test:
 * - a pending handler is not entered merely because an I/O-blocked TCB became
 *   runnable;
 * - the page-fault continuation first completes its semaphore handoff and
 *   releases the global page-cache BlockingLock;
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
 * The child publishes readiness immediately before touching an uncached file
 * mapping. The test target deliberately slows SD DMA, giving the parent time to
 * make SIGNAL_HELLO pending while that first fault is blocked. The handler
 * faults a distinct uncached mapping. It can complete only if delivery waited
 * until the first page-cache acquisition fully unwound.
 */

#include "../../../root/crt/sys.h"
#include "../../user_test.h"

#define STRESS_ITERATIONS 4
#define HANDLER_WAIT_YIELDS 32

#define CHILD_SETUP_FAILED 11
#define CHILD_HANDLER_FAILED 12

static int child_ready_sem = -1;
static char* handler_mapping = NULL;
static int primary_fault_started = 0;
static int handler_completed = 0;

static int page_faulting_handler(int signal){
  // primary_fault_started is written in user mode immediately before the
  // first mapping access, with no intervening syscall. If the signal were
  // delivered at sem_up's earlier return boundary, this phase check would
  // fail even though the handler's own mapping happened to be readable.
  if (signal != SIGNAL_HELLO || !primary_fault_started ||
      handler_mapping[0] != 'B'){
    exit(CHILD_HANDLER_FAILED);
  }

  handler_completed = 1;
  sigreturn(0);
}

static int child_main(void){
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

  // After this readiness publication returns, no further syscall lies before
  // the phase write and mapping access. The parent waits one PIT period before
  // sending, so the deliberately slow fault is already blocked rather than
  // targeting sem_up's earlier final-return boundary.
  sem_up(child_ready_sem);
  primary_fault_started = 1;
  if (primary_mapping[0] != 'A'){
    return CHILD_SETUP_FAILED;
  }

  for (int i = 0; i < HANDLER_WAIT_YIELDS && !handler_completed; ++i){
    yield();
  }

  return handler_completed ? 0 : CHILD_HANDLER_FAILED;
}

static int run_once(void){
  child_ready_sem = sem_open(0);
  if (child_ready_sem < 0){
    return 1;
  }

  int child = fork();
  if (child == 0){
    exit(child_main());
    return -1;
  }

  int failures = 0;
  failures += sem_down(child_ready_sem) != 0;
  // sem_up made this parent runnable before the child touched the mapping. By
  // sleeping once, the parent gives the child a deterministic chance to enter
  // the deliberately slow SD fault; SD_DMA_TICKS is much larger than one PIT
  // period, so the transfer remains blocked when this parent wakes.
  sleep(1);
  failures += signal_child(child, SIGNAL_HELLO) != 0;
  failures += wait_child(child) != 0;
  failures += sem_close(child_ready_sem) != 0;
  return failures;
}

int main(void){
  int failures = 0;
  for (int i = 0; i < STRESS_ITERATIONS; ++i){
    failures += run_once();
  }

  user_test_expect_eq("signals wait for the final kernel-to-user return",
    failures, 0);
  return 0;
}
