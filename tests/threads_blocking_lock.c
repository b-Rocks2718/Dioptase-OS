/*
 * Blocking lock test.
 *
 * Validates:
 * - blocking_lock serializes every increment under contention
 * - all workers eventually make progress through repeated acquire/release cycles
 *
 * How:
 * - spawn NUM_THREADS workers
 * - each worker waits for the full set to start, then increments one shared
 *   counter LOCK_ROUNDS times while holding the blocking lock
 * - the final counter must equal NUM_THREADS * LOCK_ROUNDS
 */

#include "../kernel/blocking_lock.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

#define NUM_THREADS 12
#define LOCK_ROUNDS 100

struct ThreadArg {
  int id;
  int rounds;
};

static struct BlockingLock lock;
static int started = 0;
static int finished = 0;
static int shared_counter = 0;

// Repeatedly take the lock, bump the shared counter, and yield.
static void worker_thread(void* arg) {
  struct ThreadArg* a = (struct ThreadArg*)arg;
  __atomic_fetch_add(&started, 1);

  // Wait until all workers are ready so the loop runs under contention.
  while (__atomic_load_n(&started) != NUM_THREADS) {
    yield();
  }

  for (int i = 0; i < a->rounds; i++) {
    // Take one protected step, then yield to hand the lock around.
    blocking_lock_acquire(&lock);
    shared_counter += 1;
    blocking_lock_release(&lock);
    yield();
  }

  __atomic_fetch_add(&finished, 1);
}

// Spawn the worker set and verify the shared counter reaches the expected total.
void kernel_main(void) {
  say("***blocking lock test start\n", NULL);

  blocking_lock_init(&lock);

  // Start the contending workers.
  for (int i = 0; i < NUM_THREADS; i++) {
    struct ThreadArg* arg = malloc(sizeof(struct ThreadArg));
    assert(arg != NULL, "blocking lock test: ThreadArg allocation failed.\n");
    arg->id = i;
    arg->rounds = LOCK_ROUNDS;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "blocking lock test: Fun allocation failed.\n");
    fun->func = worker_thread;
    fun->arg = arg;

    thread(fun);
  }

  while (__atomic_load_n(&finished) != NUM_THREADS) {
    yield();
  }

  // Every worker should have contributed LOCK_ROUNDS increments.
  int expected = NUM_THREADS * LOCK_ROUNDS;
  if (shared_counter != expected) {
    int args[2] = { shared_counter, expected };
    say("***blocking lock FAIL total=%d expected=%d\n", args);
    panic("blocking lock test: counter mismatch\n");
  }

  say("***blocking lock ok\n", NULL);
  say("***blocking lock test complete\n", NULL);
}
