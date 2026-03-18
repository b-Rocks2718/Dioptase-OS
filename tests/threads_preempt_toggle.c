// Preemption toggle test.
// Purpose: verify preemption_disable blocks PIT-driven rescheduling on the
// current core until re-enabled.

// Note: this test assumes single core

#include "../kernel/threads.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"
#include "../kernel/constants.h"
#include "../kernel/heap.h"

#define WORKER_ROUNDS 64
#define SPIN_ITERS 200
#define POST_ENABLE_SPINS 2000

static int target_core = -1;
static int worker_ready = 0;
static int worker_progress = 0;

// exercise preemption behavior by yielding on a designated core.
static void worker_thread(void* arg) {
  (void)arg;

  while ((int)get_core_id() != __atomic_load_n(&target_core)) {
    yield();
  }

  __atomic_store_n(&worker_ready, 1);

  for (int i = 0; i < WORKER_ROUNDS; i++) {
    __atomic_fetch_add(&worker_progress, 1);
    yield();
  }
}

// validate preemption_disable prevents PIT rescheduling on this core.
void kernel_main(void) {
  say("***preemption toggle test start\n", NULL);

  target_core = (int)get_core_id();

  struct Fun* fun = malloc(sizeof(struct Fun));
  assert(fun != NULL, "preempt toggle: Fun allocation failed.\n");
  fun->func = worker_thread;
  fun->arg = NULL;
  thread(fun);

  while (__atomic_load_n(&worker_ready) == 0) {
    yield();
  }

  int before = __atomic_load_n(&worker_progress);
  bool prev = preemption_disable();

  for (int i = 0; i < SPIN_ITERS; i++) {
    pause();
  }

  int after = __atomic_load_n(&worker_progress);
  if (after != before) {
    int args[3] = { before, after, target_core };
    say("***preemption toggle FAIL progress_before=%d progress_after=%d core=%d\n", args);
    panic("preempt toggle: worker ran while preemption disabled\n");
  }

  preemption_restore(prev);

  bool saw_progress = false;
  for (int i = 0; i < POST_ENABLE_SPINS; i++) {
    if (__atomic_load_n(&worker_progress) > before) {
      saw_progress = true;
      break;
    }
    yield();
  }

  if (!saw_progress) {
    int args[2] = { __atomic_load_n(&worker_progress), before };
    say("***preemption toggle FAIL progress=%d expected>%d\n", args);
    panic("preempt toggle: worker did not run after re-enable\n");
  }

  say("***preemption toggle ok\n", NULL);
  say("***preemption toggle test complete\n", NULL);
}
