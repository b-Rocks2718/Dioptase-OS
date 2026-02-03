// Preemption stress test: threads must advance in turn without explicit yields.

#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"
#include "../kernel/constants.h"

#define NUM_THREADS 16
#define PREEMPT_ROUNDS 10

struct ThreadArg {
  int id;
  int rounds;
};

static int started = 0;
static int finished = 0;
static int turn = 0;
static int per_thread[NUM_THREADS];
static int total_steps = 0;

static void wait_for_turn(int id) {
  while (__atomic_load_n(&turn) != id) {
    // Busy-wait until preemption runs the thread that owns the turn.
    pause();
  }
}

static void thread_worker(void* arg) {
  struct ThreadArg* a = (struct ThreadArg*)arg;
  __atomic_fetch_add(&started, 1);

  while (__atomic_load_n(&started) != NUM_THREADS) {
    // Wait for all threads to be ready (no yields).
    pause();
  }

  for (int i = 0; i < a->rounds; i++) {
    wait_for_turn(a->id);
    __atomic_fetch_add(&per_thread[a->id], 1);
    __atomic_fetch_add(&total_steps, 1);
    __atomic_store_n(&turn, (a->id + 1) % NUM_THREADS);
  }

  __atomic_fetch_add(&finished, 1);
}

static void spawn_threads(void) {
  for (int i = 0; i < NUM_THREADS; i++) {
    struct ThreadArg* arg = malloc(sizeof(struct ThreadArg));
    assert(arg != NULL, "ThreadArg allocation failed.\n");
    arg->id = i;
    arg->rounds = PREEMPT_ROUNDS;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "Fun allocation failed.\n");
    fun->func = thread_worker;
    fun->arg = arg;

    thread(fun);
  }
}

static void verify_results(void) {
  int expected = NUM_THREADS * PREEMPT_ROUNDS;
  int total = __atomic_load_n(&total_steps);
  if (total != expected) {
    int args[2] = { total, expected };
    say("***threads preempt FAIL total=%d expected=%d\n", args);
    panic("threads preempt test: total count mismatch\n");
  }

  for (int i = 0; i < NUM_THREADS; i++) {
    int count = __atomic_load_n(&per_thread[i]);
    if (count != PREEMPT_ROUNDS) {
      int args[3] = { i, count, PREEMPT_ROUNDS };
      say("***threads preempt FAIL id=%d got=%d expected=%d\n", args);
      panic("threads preempt test: per-thread count mismatch\n");
    }
  }
}

void kernel_main(void) {
  say("***threads preempt test start\n", NULL);

  spawn_threads();

  while (__atomic_load_n(&finished) != NUM_THREADS) {
    // Wait for all threads to complete (no yields).
    pause();
  }

  verify_results();

  int args[3] = { NUM_THREADS, PREEMPT_ROUNDS, __atomic_load_n(&total_steps) };
  say("***threads preempt ok threads=%d rounds=%d total=%d\n", args);
  say("***threads preempt test complete\n", NULL);
}
