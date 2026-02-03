// Barrier test.
// Purpose: verify barrier_sync blocks until all threads reach the barrier.

#include "../kernel/barrier.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

#define NUM_THREADS 8

static struct Barrier barrier;
static int started = 0;
static int finished = 0;
static int phase1_total = 0;
static int phase2_total = 0;

// Purpose: hit a barrier after phase1 and verify no early phase2 progress.
// Inputs: arg points to thread id.
// Preconditions: barrier initialized with NUM_THREADS.
// Postconditions: increments phase1_total and phase2_total once each.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static void worker_thread(void* arg) {
  int id = *(int*)arg;
  (void)id;

  __atomic_fetch_add(&started, 1);

  __atomic_fetch_add(&phase1_total, 1);

  barrier_sync(&barrier);

  int seen = __atomic_load_n(&phase1_total);
  if (seen != NUM_THREADS) {
    int args[2] = { seen, NUM_THREADS };
    say("***barrier FAIL phase1=%d expected=%d\n", args);
    panic("barrier test: phase2 started before all phase1 complete\n");
  }

  __atomic_fetch_add(&phase2_total, 1);

  __atomic_fetch_add(&finished, 1);
}

// Purpose: validate barrier semantics across multiple threads.
// Inputs: none.
// Outputs: prints pass/fail status; panics on failure.
// Preconditions: kernel mode; scheduler initialized; PIT running.
// Postconditions: phase1_total == phase2_total == NUM_THREADS.
// CPU state assumptions: kernel mode; interrupts enabled except where noted.
void kernel_main(void) {
  say("***barrier test start\n", NULL);

  barrier_init(&barrier, NUM_THREADS);

  for (int i = 0; i < NUM_THREADS; i++) {
    int* id = malloc(sizeof(int));
    assert(id != NULL, "barrier test: id allocation failed.\n");
    *id = i;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "barrier test: Fun allocation failed.\n");
    fun->func = worker_thread;
    fun->arg = id;

    thread(fun);
  }

  while (__atomic_load_n(&started) != NUM_THREADS) {
    yield();
  }

  while (__atomic_load_n(&finished) != NUM_THREADS) {
    yield();
  }

  int phase1 = __atomic_load_n(&phase1_total);
  int phase2 = __atomic_load_n(&phase2_total);
  if (phase1 != NUM_THREADS || phase2 != NUM_THREADS) {
    int args[3] = { phase1, phase2, NUM_THREADS };
    say("***barrier FAIL phase1=%d phase2=%d expected=%d\n", args);
    panic("barrier test: totals mismatch\n");
  }

  say("***barrier ok\n", NULL);
  say("***barrier test complete\n", NULL);
}
