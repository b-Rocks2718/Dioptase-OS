/*
 * Semaphore destroy cleanup test.
 *
 * Validates:
 * - destroying and freeing a semaphore reaps blocked waiters instead of waking
 *   them back into execution
 * - the cleanup attribute can safely trigger sem_free() at scope exit
 *
 * How:
 * - create NUM_WAITERS threads that block on one semaphore
 * - let a scoped cleanup hook call sem_free(), which triggers sem_destroy()
 * - yield afterward and verify no waiter resumes from sem_down()
 */

#include "../kernel/semaphore.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

#define NUM_WAITERS 6
#define POST_DESTROY_YIELDS 128

struct WaiterArg {
  struct Semaphore* sem;
};

static int started = 0;
static int awakened = 0;
static int cleanup_called = 0;

// Free the scoped semaphore when the cleanup attribute fires.
static void sem_cleanup(struct Semaphore** sem_ptr) {
  if (sem_ptr != NULL && *sem_ptr != NULL) {
    sem_free(*sem_ptr);
    *sem_ptr = NULL;
    __atomic_store_n(&cleanup_called, 1);
  }
}

// Read the current waiter count from the semaphore internals.
static unsigned sem_waiter_count(struct Semaphore* sem) {
  spin_lock_acquire(&sem->lock);
  unsigned n = sem->wait_queue.size;
  spin_lock_release(&sem->lock);
  return n;
}

// Block on the semaphore that the cleanup path will later destroy.
static void waiter_thread(void* arg) {
  struct WaiterArg* a = (struct WaiterArg*)arg;
  __atomic_fetch_add(&started, 1);
  sem_down(a->sem);
  __atomic_fetch_add(&awakened, 1);
}

// Build a blocked waiter set, then leave scope so cleanup frees the semaphore.
static void run_cleanup_destroy_case(void) {
  struct Semaphore* sem __attribute__((cleanup(sem_cleanup))) =
      malloc(sizeof(struct Semaphore));
  assert(sem != NULL, "semaphore destroy test: semaphore allocation failed.\n");
  sem_init(sem, 0);

  for (int i = 0; i < NUM_WAITERS; i++) {
    struct WaiterArg* arg = malloc(sizeof(struct WaiterArg));
    assert(arg != NULL, "semaphore destroy test: waiter arg allocation failed.\n");
    arg->sem = sem;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "semaphore destroy test: Fun allocation failed.\n");
    fun->func = waiter_thread;
    fun->arg = arg;
    thread(fun);
  }

  // Wait until every waiter is actually blocked before scope exit destroys it.
  while (__atomic_load_n(&started) != NUM_WAITERS) {
    yield();
  }

  while (sem_waiter_count(sem) != NUM_WAITERS) {
    yield();
  }
  // Scope exit triggers sem_cleanup -> sem_free -> sem_destroy.
}

// Run the cleanup case and prove blocked waiters never resume.
void kernel_main(void) {
  say("***semaphore destroy cleanup test start\n", NULL);

  run_cleanup_destroy_case();

  if (__atomic_load_n(&cleanup_called) != 1) {
    say("***semaphore destroy cleanup FAIL cleanup not called\n", NULL);
    panic("semaphore destroy cleanup test: cleanup hook not invoked\n");
  }

  // Give any wrongly resumed waiter a chance to run before checking.
  for (int i = 0; i < POST_DESTROY_YIELDS; i++) {
    yield();
  }

  if (__atomic_load_n(&awakened) != 0) {
    int args[2] = { __atomic_load_n(&awakened), 0 };
    say("***semaphore destroy cleanup FAIL awakened=%d expected=%d\n", args);
    panic("semaphore destroy cleanup test: destroyed waiters resumed\n");
  }

  say("***semaphore destroy cleanup ok\n", NULL);
  say("***semaphore destroy cleanup test complete\n", NULL);
}
