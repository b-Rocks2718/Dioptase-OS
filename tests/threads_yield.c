/*
 * Thread yield test.
 *
 * Validates:
 * - repeated yield() calls let every runnable thread make progress
 * - no per-thread or total step updates are lost under heavy handoff
 *
 * How:
 * - spawn NUM_THREADS workers
 * - each worker waits for the full set to start, then performs YIELD_ROUNDS
 *   increments with a yield between every step
 * - verify both the per-thread counters and the total counter
 */

#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"
#include "../kernel/constants.h"

#define NUM_THREADS 32
#define YIELD_ROUNDS 200

struct ThreadArg {
  int id;
  int rounds;
};

static int started = 0;
static int finished = 0;
static int per_thread[NUM_THREADS];
static int total_steps = 0;

// Take one worker through YIELD_ROUNDS yield-heavy steps.
static void thread_worker(void* arg) {
  struct ThreadArg* a = (struct ThreadArg*)arg;
  __atomic_fetch_add((int*)&started, 1);

  // Wait until all workers are ready so the hot loop runs together.
  while (__atomic_load_n((int*)&started) != NUM_THREADS) {
    yield();
  }

  for (int i = 0; i < a->rounds; i++) {
    // Record one step, then immediately hand execution away.
    per_thread[a->id] += 1;
    __atomic_fetch_add(&total_steps, 1);
    yield();
  }

  __atomic_fetch_add((int*)&finished, 1);
}

// Allocate and start the full worker set.
static void spawn_threads(void) {
  for (int i = 0; i < NUM_THREADS; i++) {
    struct ThreadArg* arg = malloc(sizeof(struct ThreadArg));
    assert(arg != NULL, "ThreadArg allocation failed.\n");
    arg->id = i;
    arg->rounds = YIELD_ROUNDS;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "Fun allocation failed.\n");
    fun->func = thread_worker;
    fun->arg = arg;

    thread(fun);
  }
}

// Check the total and per-thread counters after the run completes.
static void verify_results(void) {
  int expected = NUM_THREADS * YIELD_ROUNDS;
  int total = __atomic_load_n(&total_steps);
  if (total != expected) {
    int args[2] = { total, expected };
    say("***threads yield FAIL total=%d expected=%d\n", args);
    panic("threads yield test: total count mismatch\n");
  }

  for (int i = 0; i < NUM_THREADS; i++) {
    if (per_thread[i] != YIELD_ROUNDS) {
      int args[3] = { i, per_thread[i], YIELD_ROUNDS };
      say("***threads yield FAIL id=%d got=%d expected=%d\n", args);
      panic("threads yield test: per-thread count mismatch\n");
    }
  }
}

// Run the yield-heavy workload and verify every thread finished its rounds.
void kernel_main(void) {
  say("***threads yield test start\n", NULL);

  spawn_threads();

  say("***all threads spawned\n", NULL);

  // Wait until every worker has finished its full round count.
  while (__atomic_load_n((int*)&finished) != NUM_THREADS) {
    yield();
  }

  verify_results();

  int args[3] = { NUM_THREADS, YIELD_ROUNDS, __atomic_load_n(&total_steps) };
  say("***threads yield ok threads=%d rounds=%d total=%d\n", args);
  say("***threads yield test complete\n", NULL);
}
