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

// Worker to reuse one barrier across many rounds and detect early releases
// Assumed barrier initialized with NUM_THREADS
static void worker_thread(void* arg) {
  int id = *(int*)arg; // argument is the worker id

  // mark this thread as started
  __atomic_fetch_add(&started, 1);

  for (int round = 0; round < NUM_ROUNDS; round++) {
    if (round > 0 && __atomic_load_n(&departures[round - 1]) != NUM_THREADS) {
      // ensure that at some point during the test, 
      // we actually have the race where threads are starting the next round
      // while other threads are still finishing the previous round
      __atomic_store_n(&reuse_overlap_observed, 1);
    }

    // mark arrival of this thread for this round
    __atomic_fetch_add(&arrivals[round], 1);

    barrier_sync(&barrier);

    // threads should only depart barrier after all have arrived
    int seen = __atomic_load_n(&arrivals[round]);
    if (seen != NUM_THREADS) {
      int args[3] = { round, seen, NUM_THREADS };
      say("***barrier FAIL round=%d arrivals=%d expected=%d\n", args);
      panic("barrier test: barrier released a reused generation before all arrivals completed\n");
    }

    // slow worker yields to give other threads time to get ahead
    // this should not break the barrier
    if (id == SLOW_WORKER_ID && round + 1 < NUM_ROUNDS) {
      for (int i = 0; i < POST_BARRIER_SLOW_YIELDS; i++) {
        yield();
      }
    }

    // mark departure of this thread for this round
    __atomic_fetch_add(&departures[round], 1);
  }

  // mark this thread as finished
  __atomic_fetch_add(&finished, 1);
}

// Start reusable barrier workers
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

  // wait some time, and check that all workers started
  for (int i = 0; i < START_WAIT_BUDGET &&
                  __atomic_load_n(&started) != NUM_THREADS;
       i++) {
    yield();
  }
  if (__atomic_load_n(&started) != NUM_THREADS) {
    // workers took too long to start
    int args[2] = { __atomic_load_n(&started), NUM_THREADS };
    say("***barrier FAIL started=%d expected=%d\n", args);
    panic("barrier test: not all worker threads started\n");
  }

  // wait some time, and check that all workers finished
  for (int i = 0; i < FINISH_WAIT_BUDGET &&
                  __atomic_load_n(&finished) != NUM_THREADS;
       i++) {
    yield();
  }
  if (__atomic_load_n(&finished) != NUM_THREADS) {
    // workers took too long to finish
    int args[2] = { __atomic_load_n(&finished), NUM_THREADS };
    say("***barrier FAIL finished=%d expected=%d\n", args);
    panic("barrier test: workers did not finish reusable barrier rounds\n");
  }

  if (__atomic_load_n(&reuse_overlap_observed) != 1) {
    // reuse overlap race never occurred
    say("***barrier FAIL reuse overlap was not exercised\n", NULL);
    panic("barrier test: reuse race coverage did not execute\n");
  }

  for (int round = 0; round < NUM_ROUNDS; round++) {
    // check that for every round, all threads arrived and departed as expected
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
