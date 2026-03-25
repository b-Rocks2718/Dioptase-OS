/*
 * Thread race stress test.
 *
 * Validates:
 * - one TCB never runs concurrently on multiple cores
 * - repeated yields still let every worker make forward progress
 * - concurrent heap churn does not corrupt live allocations
 *
 * How:
 * - spawn NUM_THREADS workers and wait until all of them are ready
 * - each worker marks itself "in step" before touching shared counters; seeing
 *   that marker already set means the same logical thread ran twice at once
 * - workers periodically allocate, fill, yield on, verify, and free heap blocks
 *   to mix scheduler races with allocator reuse
 * - the final check verifies that every worker and the global total hit the
 *   exact expected round counts
 */

#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"
#include "../kernel/constants.h"

#define NUM_THREADS 32
#define ROUNDS 100
#define ALLOC_INTERVAL 13
#define ALLOC_MIN_WORDS 4
#define ALLOC_WORD_STRIDE 4
#define YIELD_SPINS 1
#define PATTERN_SEED 0x6D7A

struct ThreadArg {
  int id;
  int rounds;
};

static int started = 0;
static int finished = 0;
static int total_steps = 0;
static int per_thread[NUM_THREADS];
static int in_step[NUM_THREADS];

// Wait until every worker has reached the starting line.
static void wait_for_all_threads(void) {
  while (__atomic_load_n(&started) != NUM_THREADS) {
    yield();
  }
}

// Fill one allocation with a deterministic pattern for later verification.
static void fill_words(unsigned* p, unsigned words, unsigned pattern) {
  for (unsigned i = 0; i < words; i++) {
    p[i] = pattern;
  }
}

// Verify that a live allocation still holds the pattern written by this worker.
static void check_words(unsigned* p, unsigned words, unsigned pattern, int id, int round) {
  for (unsigned i = 0; i < words; i++) {
    if (p[i] != pattern) {
      int args[4] = { id, round, (int)i, (int)pattern };
      say("***threads race stress FAIL id=%d round=%d word=%d pattern=0x%X\n", args);
      panic("threads race stress: heap pattern mismatch\n");
    }
  }
}

// Periodically churn the heap to mix allocator reuse into the scheduler stress.
static void maybe_alloc_work(int id, int round) {
  if ((round % ALLOC_INTERVAL) != 0) {
    return;
  }

  unsigned words = ALLOC_MIN_WORDS + ((id + round) % ALLOC_WORD_STRIDE);
  unsigned bytes = words * 4;
  unsigned* p = (unsigned*)malloc(bytes);
  if (p == NULL) {
    int args[3] = { id, round, (int)bytes };
    say("***threads race stress FAIL id=%d round=%d bytes=%d\n", args);
    panic("threads race stress: malloc failed\n");
  }

  unsigned pattern = ((unsigned)id << 16) ^ (unsigned)round ^ PATTERN_SEED;
  fill_words(p, words, pattern);

  for (int i = 0; i < YIELD_SPINS; i++) {
    // Yield while the block is live so concurrent heap traffic has time to overlap.
    yield();
  }

  check_words(p, words, pattern, id, round);
  free(p);
}

// Repeatedly enter a critical test step and fail if this logical thread overlaps itself.
static void thread_worker(void* arg) {
  struct ThreadArg* a = (struct ThreadArg*)arg;
  __atomic_fetch_add(&started, 1);

  wait_for_all_threads();

  for (int i = 0; i < a->rounds; i++) {
    // If this marker is already set, the same TCB is executing on two cores at once.
    int prev = __atomic_exchange_n(&in_step[a->id], 1);
    if (prev != 0) {
      int args[2] = { a->id, i };
      say("***threads race stress FAIL id=%d round=%d\n", args);
      panic("threads race stress: thread running concurrently\n");
    }

    __atomic_fetch_add(&per_thread[a->id], 1);
    __atomic_fetch_add(&total_steps, 1);

    maybe_alloc_work(a->id, i);

    for (int y = 0; y < YIELD_SPINS; y++) {
      yield();
    }

    __atomic_store_n(&in_step[a->id], 0);
  }

  __atomic_fetch_add(&finished, 1);
}

// Allocate and queue the full worker set.
static void spawn_threads(void) {
  for (int i = 0; i < NUM_THREADS; i++) {
    struct ThreadArg* arg = malloc(sizeof(struct ThreadArg));
    assert(arg != NULL, "ThreadArg allocation failed.\n");
    arg->id = i;
    arg->rounds = ROUNDS;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "Fun allocation failed.\n");
    fun->func = thread_worker;
    fun->arg = arg;

    thread(fun);
  }
}

// Check that every worker and the total counter reached the planned round count.
static void verify_results(void) {
  int expected = NUM_THREADS * ROUNDS;
  int total = __atomic_load_n(&total_steps);
  if (total != expected) {
    int args[2] = { total, expected };
    say("***threads race stress FAIL total=%d expected=%d\n", args);
    panic("threads race stress: total count mismatch\n");
  }

  for (int i = 0; i < NUM_THREADS; i++) {
    int count = __atomic_load_n(&per_thread[i]);
    if (count != ROUNDS) {
      int args[3] = { i, count, ROUNDS };
      say("***threads race stress FAIL id=%d got=%d expected=%d\n", args);
      panic("threads race stress: per-thread count mismatch\n");
    }
  }
}

void kernel_main(void) {
  say("***threads race stress test start\n", NULL);

  spawn_threads();

  while (__atomic_load_n(&finished) != NUM_THREADS) {
    yield();
  }

  verify_results();

  int args[3] = { NUM_THREADS, ROUNDS, __atomic_load_n(&total_steps) };
  say("***threads race stress ok threads=%d rounds=%d total=%d\n", args);
  say("***threads race stress test complete\n", NULL);
}
