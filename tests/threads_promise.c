// Promise test.
// Purpose: verify that promise_get blocks until promise_set and that all
// waiters observe the same value.

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

static void waiter_thread(void* arg) {
  int id = *(int*)arg;
  __atomic_fetch_add(&started, 1);
  results[id] = promise_get(&promise);
  __atomic_fetch_add(&done, 1);
}

void kernel_main(void) {
  say("***promise test start\n", NULL);

  promise_init(&promise);

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

  for (int i = 0; i < PRE_SET_YIELDS; i++) {
    yield();
  }

  if (__atomic_load_n(&done) != 0) {
    int args[2] = { __atomic_load_n(&done), 0 };
    say("***promise FAIL done=%d expected=%d\n", args);
    panic("promise test: waiters ran before set\n");
  }

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
