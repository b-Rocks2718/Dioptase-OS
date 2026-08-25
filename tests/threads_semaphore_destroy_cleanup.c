/*
 * Quiescent semaphore destruction and cleanup test.
 *
 * Validates:
 * - semaphore destruction does not manufacture terminal TCB transitions
 * - every blocked waiter is woken normally and returns from sem_down()
 * - active_operations covers blocked continuations and reaches zero only
 *   after every sem_down() continuation has resumed
 * - a scoped cleanup hook may call sem_free() once the owner has prevented new
 *   operations and waited for every existing operation to finish
 *
 * How:
 * - create NUM_WAITERS threads that block on one semaphore
 * - wait until all are published in the semaphore queue and confirm each live
 *   sem_down() is represented in active_operations
 * - wake each waiter normally, wait for all continuations to return, then let
 *   scope cleanup destroy/free the now-quiescent semaphore
 */

#include "../kernel/semaphore.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

#define NUM_WAITERS 6

struct WaiterArg {
  struct Semaphore* sem;
};

static int started = 0;
static int returned = 0;
static int cleanup_called = 0;

// Free the scoped semaphore when the cleanup attribute fires. The surrounding
// scope establishes quiescence before this hook is allowed to run.
static void sem_cleanup(struct Semaphore** sem_ptr) {
  if (sem_ptr != NULL && *sem_ptr != NULL) {
    sem_free(*sem_ptr);
    *sem_ptr = NULL;
    __atomic_store_n(&cleanup_called, 1);
  }
}

// Snapshot queue and operation state under the semaphore's CLH lock.
static void sem_state(struct Semaphore* sem, int* waiters, int* active) {
  clh_lock_acquire(&sem->lock);
  *waiters = sem->wait_queue.size;
  *active = __atomic_load_n(&sem->active_operations);
  clh_lock_release(&sem->lock);
}

// Block until kernel_main transfers one real semaphore permit.
static void waiter_thread(void* arg) {
  struct WaiterArg* a = (struct WaiterArg*)arg;
  __atomic_fetch_add(&started, 1);
  sem_down(a->sem);

  // sem_down() has already retired its active-operation reference before it
  // returns here. Publish completion afterward so the owner can prove that no
  // synchronization continuation still touches the object.
  __atomic_fetch_add(&returned, 1);
}

// Build a blocked waiter set, wake/join it, then leave scope for cleanup.
static void run_quiescent_cleanup_case(void) {
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

  while (__atomic_load_n(&started) != NUM_WAITERS) {
    yield();
  }

  int waiters = 0;
  int active = 0;
  do {
    sem_state(sem, &waiters, &active);
    if (waiters != NUM_WAITERS) {
      yield();
    }
  } while (waiters != NUM_WAITERS);

  if (active != NUM_WAITERS) {
    int args[2] = {active, NUM_WAITERS};
    say("***semaphore destroy cleanup FAIL active=%d expected=%d\n", args);
    panic("semaphore destroy cleanup test: blocked sem_down operations were not tracked\n");
  }

  for (int i = 0; i < NUM_WAITERS; i++) {
    sem_up(sem);
  }

  while (__atomic_load_n(&returned) != NUM_WAITERS) {
    yield();
  }

  sem_state(sem, &waiters, &active);
  if (waiters != 0 || active != 0) {
    int args[2] = {waiters, active};
    say("***semaphore destroy cleanup FAIL waiters=%d active=%d expected=0/0\n",
      args);
    panic("semaphore destroy cleanup test: semaphore was not quiescent after waiter join\n");
  }

  // Scope exit triggers sem_cleanup -> sem_free only after quiescence.
}

void kernel_main(void) {
  say("***semaphore destroy cleanup test start\n", NULL);

  run_quiescent_cleanup_case();

  if (__atomic_load_n(&cleanup_called) != 1) {
    say("***semaphore destroy cleanup FAIL cleanup not called\n", NULL);
    panic("semaphore destroy cleanup test: cleanup hook not invoked\n");
  }

  say("***semaphore destroy cleanup ok\n", NULL);
  say("***semaphore destroy cleanup test complete\n", NULL);
}
