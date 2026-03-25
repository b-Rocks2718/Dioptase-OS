/*
 * Promise test.
 *
 * Validates:
 * - promise_get blocks until promise_set publishes a value
 * - every waiter receives the exact same published pointer
 *
 * How:
 * - start NUM_WAITERS threads that immediately call promise_get()
 * - yield for a short window to prove none of them returns early
 * - publish one stack value with promise_set()
 * - verify every waiter records the same address
 */

#include "../kernel/promise.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

#define NUM_WAITERS 6
#define PRE_SET_YIELDS 32

static struct Promise promise;
static int started = 0;
static int done = 0;
static void* results[NUM_WAITERS];

// Block on the shared promise and store the value this waiter receives.
static void waiter_thread(void* arg) {
  int id = *(int*)arg;
  __atomic_fetch_add(&started, 1);
  // Wait until main publishes the promise value.
  results[id] = promise_get(&promise);
  __atomic_fetch_add(&done, 1);
}

// Start the waiter set, publish one value, and verify every waiter sees it.
void kernel_main(void) {
  say("***promise test start\n", NULL);

  promise_init(&promise);

  // Start the waiters that will block on the unset promise.
  for (int i = 0; i < NUM_WAITERS; i++) {
    int* id = malloc(sizeof(int));
    assert(id != NULL, "promise test: id allocation failed.\n");
    *id = i;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "promise test: Fun allocation failed.\n");
    fun->func = waiter_thread;
    fun->arg = id;
    thread(fun);
  }

  while (__atomic_load_n(&started) != NUM_WAITERS) {
    yield();
  }

  // Give the waiters time to prove they really block before set().
  for (int i = 0; i < PRE_SET_YIELDS; i++) {
    yield();
  }

  if (__atomic_load_n(&done) != 0) {
    int args[2] = { __atomic_load_n(&done), 0 };
    say("***promise FAIL done=%d expected=%d\n", args);
    panic("promise test: waiters ran before set\n");
  }

  // Publish one value and wait for everyone to observe it.
  int value = 0x1234;
  promise_set(&promise, &value);

  while (__atomic_load_n(&done) != NUM_WAITERS) {
    yield();
  }

  for (int i = 0; i < NUM_WAITERS; i++) {
    if ((unsigned)results[i] != (unsigned)&value) {
      int args[2] = { i, (int)(unsigned)results[i] };
      say("***promise FAIL id=%d value=0x%X\n", args);
      panic("promise test: waiter saw incorrect value\n");
    }
  }

  say("***promise ok\n", NULL);
  say("***promise test complete\n", NULL);
}
