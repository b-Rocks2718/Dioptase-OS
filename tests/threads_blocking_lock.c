// Blocking lock test.
// Purpose: validate that blocking_lock enforces mutual exclusion under contention.

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

// Purpose: increment shared_counter while holding the blocking lock.
// Inputs: arg points to ThreadArg.
// Preconditions: kernel mode; blocking lock initialized.
// Postconditions: shared_counter incremented rounds times by this thread.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static void worker_thread(void* arg) {
  struct ThreadArg* a = (struct ThreadArg*)arg;
  __atomic_fetch_add(&started, 1);

  while (__atomic_load_n(&started) != NUM_THREADS) {
    yield();
  }

  for (int i = 0; i < a->rounds; i++) {
    blocking_lock_get(&lock);
    shared_counter += 1;
    blocking_lock_release(&lock);
    yield();
  }

  __atomic_fetch_add(&finished, 1);
}

// Purpose: stress blocking_lock under multi-threaded contention.
// Inputs: none.
// Outputs: prints pass/fail status; panics on failure.
// Preconditions: kernel mode; scheduler initialized; PIT running.
// Postconditions: shared_counter == NUM_THREADS * LOCK_ROUNDS.
// CPU state assumptions: kernel mode; interrupts enabled except where noted.
void kernel_main(void) {
  say("***blocking lock test start\n", NULL);

  blocking_lock_init(&lock);

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

  int expected = NUM_THREADS * LOCK_ROUNDS;
  if (shared_counter != expected) {
    int args[2] = { shared_counter, expected };
    say("***blocking lock FAIL total=%d expected=%d\n", args);
    panic("blocking lock test: counter mismatch\n");
  }

  say("***blocking lock ok\n", NULL);
  say("***blocking lock test complete\n", NULL);
}
