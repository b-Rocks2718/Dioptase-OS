/*
 * Barrier test.
 *
 * Validates:
 * - barrier_sync() does not release a generation until all participants arrive
 * - the same barrier can be reused across many generations by the same thread set
 * - fast threads racing into the next generation cannot leak through early while
 *   a slow thread is still doing post-barrier work from the previous round
 *
 * How:
 * - spawn NUM_THREADS workers that loop over NUM_ROUNDS barrier generations
 * - worker 0 yields after every barrier release so the other workers aggressively
 *   reuse the same barrier before the slow worker starts the next round
 * - each worker checks that the current round's arrival count is already
 *   NUM_THREADS when barrier_sync() returns; any smaller count means a reuse race
 *   released the next generation early
 */

#include "../kernel/barrier.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

#define NUM_THREADS 8
#define NUM_ROUNDS 32
#define SLOW_WORKER_ID 0
#define POST_BARRIER_SLOW_YIELDS 64
#define START_WAIT_BUDGET 100000
#define FINISH_WAIT_BUDGET 200000

static struct Barrier barrier;
static int started = 0;
static int finished = 0;
static int reuse_overlap_observed = 0;
static int arrivals[NUM_ROUNDS];
static int departures[NUM_ROUNDS];

// Purpose: reuse one barrier across many rounds and detect early releases.
// Inputs: arg points to the worker id.
// Preconditions: barrier initialized with NUM_THREADS.
// Postconditions: increments arrivals[round] and departures[round] once per round.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static void worker_thread(void* arg) {
  int id = *(int*)arg;

  __atomic_fetch_add(&started, 1);

  for (int round = 0; round < NUM_ROUNDS; round++) {
    if (round > 0 && __atomic_load_n(&departures[round - 1]) != NUM_THREADS) {
      __atomic_store_n(&reuse_overlap_observed, 1);
    }

    __atomic_fetch_add(&arrivals[round], 1);

    barrier_sync(&barrier);

    int seen = __atomic_load_n(&arrivals[round]);
    if (seen != NUM_THREADS) {
      int args[3] = { round, seen, NUM_THREADS };
      say("***barrier FAIL round=%d arrivals=%d expected=%d\n", args);
      panic("barrier test: barrier released a reused generation before all arrivals completed\n");
    }

    if (id == SLOW_WORKER_ID && round + 1 < NUM_ROUNDS) {
      for (int i = 0; i < POST_BARRIER_SLOW_YIELDS; i++) {
        yield();
      }
    }

    __atomic_fetch_add(&departures[round], 1);
  }

  __atomic_fetch_add(&finished, 1);
}

// Purpose: start reusable barrier workers and validate all generations complete.
// Inputs: none.
// Outputs: prints pass/fail status; panics on failure.
// Preconditions: kernel mode; scheduler initialized; PIT running.
// Postconditions: every round records NUM_THREADS arrivals and departures.
// CPU state assumptions: kernel mode; interrupts enabled except where noted.
static void spawn_worker(int id) {
  int* arg = malloc(sizeof(int));
  assert(arg != NULL, "barrier test: worker id allocation failed.\n");
  *arg = id;

  struct Fun* fun = malloc(sizeof(struct Fun));
  assert(fun != NULL, "barrier test: Fun allocation failed.\n");
  fun->func = worker_thread;
  fun->arg = arg;

  thread(fun);
}

void kernel_main(void) {
  say("***barrier test start\n", NULL);

  barrier_init(&barrier, NUM_THREADS);

  for (int i = 0; i < NUM_THREADS; i++) {
    spawn_worker(i);
  }

  for (int i = 0; i < START_WAIT_BUDGET &&
                  __atomic_load_n(&started) != NUM_THREADS;
       i++) {
    yield();
  }
  if (__atomic_load_n(&started) != NUM_THREADS) {
    int args[2] = { __atomic_load_n(&started), NUM_THREADS };
    say("***barrier FAIL started=%d expected=%d\n", args);
    panic("barrier test: not all worker threads started\n");
  }

  for (int i = 0; i < FINISH_WAIT_BUDGET &&
                  __atomic_load_n(&finished) != NUM_THREADS;
       i++) {
    yield();
  }
  if (__atomic_load_n(&finished) != NUM_THREADS) {
    int args[2] = { __atomic_load_n(&finished), NUM_THREADS };
    say("***barrier FAIL finished=%d expected=%d\n", args);
    panic("barrier test: workers did not finish reusable barrier rounds\n");
  }

  if (__atomic_load_n(&reuse_overlap_observed) != 1) {
    say("***barrier FAIL reuse overlap was not exercised\n", NULL);
    panic("barrier test: reuse race coverage did not execute\n");
  }

  for (int round = 0; round < NUM_ROUNDS; round++) {
    int arrival_count = __atomic_load_n(&arrivals[round]);
    int departure_count = __atomic_load_n(&departures[round]);
    if (arrival_count != NUM_THREADS || departure_count != NUM_THREADS) {
      int args[4] = { round, arrival_count, departure_count, NUM_THREADS };
      say("***barrier FAIL round=%d arrivals=%d departures=%d expected=%d\n", args);
      panic("barrier test: reusable barrier round totals mismatch\n");
    }
  }

  barrier_destroy(&barrier);
  say("***barrier ok\n", NULL);
  say("***barrier test complete\n", NULL);
}
