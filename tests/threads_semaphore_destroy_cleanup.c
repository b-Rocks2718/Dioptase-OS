// Semaphore destroy/free test.
// Purpose: verify that destroying a semaphore reaps blocked waiters and that
// __attribute__((cleanup)) can safely trigger sem_free on scope exit.

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

// Purpose: auto-cleanup hook for a local Semaphore* variable.
// Inputs: sem_ptr points to a local pointer variable.
// Preconditions: sem_ptr is non-NULL.
// Postconditions: sem_free called once for non-NULL pointer.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static void sem_cleanup(struct Semaphore** sem_ptr) {
  if (sem_ptr != NULL && *sem_ptr != NULL) {
    sem_free(*sem_ptr);
    *sem_ptr = NULL;
    __atomic_store_n(&cleanup_called, 1);
  }
}

// Purpose: read semaphore waiter count under semaphore lock.
// Inputs: sem points to a semaphore.
// Outputs: current waiter count.
// Preconditions: sem is non-NULL.
// Postconditions: none.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static unsigned sem_waiter_count(struct Semaphore* sem) {
  spin_lock_acquire(&sem->lock);
  unsigned n = sem->wait_queue.size;
  spin_lock_release(&sem->lock);
  return n;
}

// Purpose: block on a semaphore that should later be destroyed.
// Inputs: arg points to WaiterArg.
// Preconditions: arg and arg->sem are non-NULL.
// Postconditions: increments started before blocking.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static void waiter_thread(void* arg) {
  struct WaiterArg* a = (struct WaiterArg*)arg;
  __atomic_fetch_add(&started, 1);
  sem_down(a->sem);
  __atomic_fetch_add(&awakened, 1);
}

// Purpose: build a waiter set and rely on cleanup attribute to free semaphore.
// Inputs: none.
// Preconditions: kernel mode.
// Postconditions: all waiters are queued on the semaphore before return.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
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

  while (__atomic_load_n(&started) != NUM_WAITERS) {
    yield();
  }

  while (sem_waiter_count(sem) != NUM_WAITERS) {
    yield();
  }
  // Scope exit triggers sem_cleanup -> sem_free -> sem_destroy.
}

// Purpose: validate sem_destroy + sem_free behavior through cleanup attribute.
// Inputs: none.
// Outputs: prints pass/fail status; panics on failure.
// Preconditions: kernel mode; scheduler initialized; PIT running.
// Postconditions: blocked waiters are destroyed, not resumed.
// CPU state assumptions: kernel mode; interrupts enabled except where noted.
void kernel_main(void) {
  say("***semaphore destroy cleanup test start\n", NULL);

  run_cleanup_destroy_case();

  if (__atomic_load_n(&cleanup_called) != 1) {
    say("***semaphore destroy cleanup FAIL cleanup not called\n", NULL);
    panic("semaphore destroy cleanup test: cleanup hook not invoked\n");
  }

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
