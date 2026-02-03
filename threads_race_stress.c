// Thread race stress test.
// What this test does, step-by-step:
// 1) Spawns many kernel threads and waits until all have started.
// 2) Each thread runs for a fixed number of rounds. In each round it:
//    - Marks itself "in step" using an atomic exchange, and fails if it was
//      already marked. This detects the same thread running concurrently
//      (e.g., being scheduled on two cores at once).
//    - Increments per-thread and total counters to validate progress.
//    - Periodically allocates a small heap block, fills it with a unique
//      pattern derived from (thread id, round), yields a few times to encourage
//      preemption, then verifies the pattern before freeing.
//    - Yields again to maximize context switches and interleavings.
// 3) After all threads finish, the test checks that every per-thread counter
//    and the total counter match the expected values.
// Failure signals:
// - "thread running concurrently" => the same TCB executed in parallel.
// - "heap pattern mismatch" => memory corruption or stale pointers.
// - "count mismatch" => lost updates or missed rounds.

#include "kernel/threads.h"
#include "kernel/heap.h"
#include "kernel/print.h"
#include "kernel/debug.h"
#include "kernel/machine.h"
#include "kernel/constants.h"

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

static void wait_for_all_threads(void) {
  while (__atomic_load_n(&started) != NUM_THREADS) {
    yield();
  }
}

static void fill_words(unsigned* p, unsigned words, unsigned pattern) {
  for (unsigned i = 0; i < words; i++) {
    p[i] = pattern;
  }
}

static void check_words(unsigned* p, unsigned words, unsigned pattern, int id, int round) {
  for (unsigned i = 0; i < words; i++) {
    if (p[i] != pattern) {
      int args[4] = { id, round, (int)i, (int)pattern };
      say("***threads race stress FAIL id=%d round=%d word=%d pattern=0x%X\n", args);
      panic("threads race stress: heap pattern mismatch\n");
    }
  }
}

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
    yield();
  }

  check_words(p, words, pattern, id, round);
  free(p);
}

static void thread_worker(void* arg) {
  struct ThreadArg* a = (struct ThreadArg*)arg;
  __atomic_fetch_add(&started, 1);

  wait_for_all_threads();

  for (int i = 0; i < a->rounds; i++) {
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
