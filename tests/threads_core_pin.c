/*
 * Core pinning test.
 *
 * Validates:
 * - core_pin() keeps a blocked thread bound to the core where it pinned itself
 *   when later wakeups make it runnable again
 *
 * How:
 * - the worker calls core_pin() and records its startup core
 * - main wakes the worker repeatedly with a semaphore
 * - each wakeup path goes through sem_up() -> global_queue_add()
 * - the worker asserts that every resumed execution still happens on the
 *   original pinned core
 */

#include "../kernel/semaphore.h"
#include "../kernel/threads.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"
#include "../kernel/heap.h"

#define WAKE_ROUNDS 128
#define READY_WAIT_BUDGET 4096

static struct Semaphore wake_sem;
static struct Semaphore done_sem;

static int worker_ready = 0;
static int worker_pinned_core = -1;
static int worker_runs = 0;

// Pin to the startup core and verify every later wakeup resumes there again.
static void worker_thread(void* arg) {
  (void)arg;

  core_pin();

  // Record and pin the first core this worker runs on.
  __atomic_store_n(&worker_pinned_core, (int)get_core_id());

  __atomic_store_n(&worker_ready, 1);

  for (int round = 0; round < WAKE_ROUNDS; round++) {
    sem_down(&wake_sem);

    // Every resume after sem_down() should stay on the pinned core.
    int resume_core = (int)get_core_id();
    if (resume_core != __atomic_load_n(&worker_pinned_core)) {
      int args[3] = {
        round,
        resume_core,
        __atomic_load_n(&worker_pinned_core)
      };
      say("***core pin FAIL round=%d core=%d expected=%d\n", args);
      panic("core pin test: pinned worker resumed on the wrong core\n");
    }

    __atomic_fetch_add(&worker_runs, 1);
    sem_up(&done_sem);
  }
}

// Allocate and start the single pinned worker.
static void spawn_worker(void) {
  struct Fun* fun = malloc(sizeof(struct Fun));
  assert(fun != NULL, "core pin test: Fun allocation failed.\n");
  fun->func = worker_thread;
  fun->arg = NULL;
  thread(fun);
}

// Wake the pinned worker repeatedly and verify it never migrates.
void kernel_main(void) {
  say("***core pin test start\n", NULL);

  sem_init(&wake_sem, 0);
  sem_init(&done_sem, 0);

  spawn_worker();

  // Wait until the worker has pinned itself and recorded its home core.
  bool ready = false;
  for (int i = 0; i < READY_WAIT_BUDGET; i++) {
    if (__atomic_load_n(&worker_ready) != 0) {
      ready = true;
      break;
    }
    yield();
  }

  if (!ready) {
    say("***core pin FAIL worker did not start\n", NULL);
    panic("core pin test: worker never reached its pinned setup point\n");
  }

  if (__atomic_load_n(&worker_pinned_core) < 0) {
    int args[1] = { __atomic_load_n(&worker_pinned_core) };
    say("***core pin FAIL invalid pinned_core=%d\n", args);
    panic("core pin test: worker recorded an invalid pinned core\n");
  }

  // Each wakeup should round-trip through semaphores on the same core.
  for (int i = 0; i < WAKE_ROUNDS; i++) {
    sem_up(&wake_sem);
    sem_down(&done_sem);
  }

  if (__atomic_load_n(&worker_runs) != WAKE_ROUNDS) {
    int args[2] = { __atomic_load_n(&worker_runs), WAKE_ROUNDS };
    say("***core pin FAIL runs=%d expected=%d\n", args);
    panic("core pin test: worker did not complete all wakeup rounds\n");
  }

  sem_destroy(&wake_sem);
  sem_destroy(&done_sem);

  say("***core pin ok\n", NULL);
  say("***core pin test complete\n", NULL);
}
