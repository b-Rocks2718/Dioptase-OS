/*
 * Preemption toggle test.
 *
 * Validates:
 * - preemption_disable() blocks PIT-driven involuntary rescheduling on the
 *   current core
 * - preemption_restore() lets another runnable thread on that same core make
 *   progress again
 *
 * How:
 * - pin kernel_main() to its current core
 * - spawn one worker that retries placement through semaphore block/wake
 *   cycles until it actually runs on that pinned core
 * - once placed, the worker only uses yield(), so it stays on the target
 *   core's local ready queue
 * - while preemption is disabled, main spins without yielding and checks that
 *   the worker's progress counter does not change
 */

#include "../kernel/semaphore.h"
#include "../kernel/threads.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"
#include "../kernel/heap.h"

#define WORKER_ROUNDS 64
#define PLACEMENT_RETRY_BUDGET 4096
#define PREEMPT_DISABLE_PAUSES 128
#define POST_ENABLE_YIELD_BUDGET 4096

static struct Semaphore placement_sem;
static struct Semaphore placement_done_sem;

static int target_core = -1;
static int worker_ready = 0;
static int worker_progress = 0;
static int worker_retry_count = 0;

// Retry until the worker runs on target_core, then pin it there and yield.
static void worker_thread(void* arg) {
  (void)arg;

  while (true) {
    sem_down(&placement_sem);

    if ((int)get_core_id() != __atomic_load_n(&target_core)) {
      // Tell main to try again until the worker is placed on the pinned core.
      __atomic_fetch_add(&worker_retry_count, 1);
      sem_up(&placement_done_sem);
      continue;
    }

    core_pin();
    __atomic_store_n(&worker_ready, 1);
    sem_up(&placement_done_sem);
    break;
  }

  for (int i = 0; i < WORKER_ROUNDS; i++) {
    __atomic_fetch_add(&worker_progress, 1);
    yield();
  }
}

// Allocate and start the worker used to probe same-core scheduling.
static void spawn_worker(void) {
  struct Fun* fun = malloc(sizeof(struct Fun));
  assert(fun != NULL, "preempt toggle: Fun allocation failed.\n");
  fun->func = worker_thread;
  fun->arg = NULL;
  thread(fun);
}

void kernel_main(void) {
  say("***preemption toggle test start\n", NULL);

  sem_init(&placement_sem, 0);
  sem_init(&placement_done_sem, 0);

  core_pin();
  __atomic_store_n(&target_core, (int)get_core_id());

  spawn_worker();

  // Drive block/wake retries until the worker actually lands on the pinned core.
  bool placed = false;
  for (int i = 0; i < PLACEMENT_RETRY_BUDGET; i++) {
    sem_up(&placement_sem);
    sem_down(&placement_done_sem);
    if (__atomic_load_n(&worker_ready) != 0) {
      placed = true;
      break;
    }
  }

  if (!placed) {
    int args[2] = {
      __atomic_load_n(&target_core),
      __atomic_load_n(&worker_retry_count)
    };
    say("***preemption toggle FAIL target_core=%d retries=%d\n", args);
    panic("preempt toggle: worker never reached the target core\n");
  }

  int before = __atomic_load_n(&worker_progress);
  bool prev = preemption_disable();

  for (int i = 0; i < PREEMPT_DISABLE_PAUSES; i++) {
    // Stay runnable without yielding so only involuntary preemption could run the worker.
    pause();
  }

  int after = __atomic_load_n(&worker_progress);
  if (after != before) {
    int args[3] = { before, after, __atomic_load_n(&target_core) };
    say("***preemption toggle FAIL progress_before=%d progress_after=%d core=%d\n", args);
    panic("preempt toggle: worker ran while preemption was disabled on its core\n");
  }

  preemption_restore(prev);

  // Once preemption is restored, a same-core runnable worker should move again.
  bool saw_progress = false;
  for (int i = 0; i < POST_ENABLE_YIELD_BUDGET; i++) {
    if (__atomic_load_n(&worker_progress) > before) {
      saw_progress = true;
      break;
    }
    yield();
  }

  if (!saw_progress) {
    int args[2] = { __atomic_load_n(&worker_progress), before };
    say("***preemption toggle FAIL progress=%d expected>%d\n", args);
    panic("preempt toggle: worker did not run after preemption was restored\n");
  }

  core_unpin();
  sem_destroy(&placement_sem);
  sem_destroy(&placement_done_sem);

  say("***preemption toggle ok\n", NULL);
  say("***preemption toggle test complete\n", NULL);
}
