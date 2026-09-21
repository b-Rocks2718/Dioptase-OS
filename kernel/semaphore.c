#include "semaphore.h"
#include "heap.h"
#include "per_core.h"
#include "threads.h"
#include "interrupts.h"
#include "scheduler.h"
#include "print.h"
#include "debug.h"

// Initialize a semaphore with a non-negative count and empty waiter queue.
void sem_init(struct Semaphore* sem, int initial_count){
  assert(sem != NULL, "sem_init: semaphore is NULL.\n");
  if (initial_count < 0) {
    int args[2] = {(int)sem, initial_count};
    say("| semaphore init rejected sem=0x%X count=%d\n", args);
    panic("sem_init: initial count must be non-negative and fit in signed int.\n");
  }
  clh_lock_init(&sem->lock);
  sem->count = initial_count;
  queue_init(&sem->wait_queue);
  __atomic_store_n(&sem->active_operations, 0);
}

// callback for when sem_down blocks
// adds the current thread to the semaphore wait queue,
// or if the count is > 0, just decrements the count and adds the thread to the ready queue
static void sem_add(void* arg){
  int** args = (int**)arg;
  struct Semaphore* sem = (struct Semaphore*)args[0];
  struct TCB* tcb = (struct TCB*)args[1];

  clh_lock_acquire(&sem->lock);

  if (sem->count > 0){
    sem->count--;
    clh_lock_release(&sem->lock);
    scheduler_wake_thread(tcb);
  } else {
    queue_add(&sem->wait_queue, tcb);
    clh_lock_release(&sem->lock);
  }
}

// Decrement the semaphore or block until a permit exists.
void sem_down(struct Semaphore* sem){
  assert(sem != NULL, "sem_down: semaphore is NULL.\n");

  /*
   * This reference begins before the first CLH exchange and remains live
   * across block(). Consequently sem_destroy() can diagnose both a contender
   * already linked into the CLH tail and the otherwise invisible interval
   * before sem_add() publishes the TCB in wait_queue.
   *
   * Preconditions: the owner has not begun destruction and guarantees the
   * Semaphore storage remains allocated until this operation returns.
   * Postcondition: active_operations is decremented only after this caller
   * either consumed a permit or resumed from a transferred permit.
   */
  __atomic_fetch_add(&sem->active_operations, 1);

  // if the count is > 0, decrement it and return, 
  // otherwise block until another thread calls sem_up
  clh_lock_acquire(&sem->lock);

  if (sem->count > 0){
    sem->count--;
    clh_lock_release(&sem->lock);
    __atomic_fetch_add(&sem->active_operations, -1);
    return;
  }
  
  clh_lock_release(&sem->lock);

  int was = interrupts_disable();

  struct TCB* current_tcb = get_current_tcb();
  
  int* args[2] = { (int*)sem, (int*)current_tcb };
  block(was, (void (*)(void *))sem_add, (void*)(args), true);

  // sem_up() transferred one permit before waking this TCB. The operation is
  // no longer live only after the suspended sem_down() continuation resumes.
  __atomic_fetch_add(&sem->active_operations, -1);
}

// Decrement without blocking; return false when no permit is available.
bool sem_try_down(struct Semaphore* sem){
  assert(sem != NULL, "sem_try_down: semaphore is NULL.\n");
  __atomic_fetch_add(&sem->active_operations, 1);
  clh_lock_acquire(&sem->lock);

  if (sem->count > 0){
    sem->count--;
    clh_lock_release(&sem->lock);
    __atomic_fetch_add(&sem->active_operations, -1);
    return true;
  }

  clh_lock_release(&sem->lock);
  __atomic_fetch_add(&sem->active_operations, -1);
  return false;
}

// Attempt to publish one permit without blocking.
bool sem_try_up(struct Semaphore* sem){
  assert(sem != NULL, "sem_try_up: semaphore is NULL.\n");
  __atomic_fetch_add(&sem->active_operations, 1);

  // try to wake up a waiting thread, if there are none, increment the count
  clh_lock_acquire(&sem->lock);

  struct TCB* wakeup = queue_remove(&sem->wait_queue);
  bool success = true;
  if (wakeup == NULL){
    if (sem->count == INT_MAX) {
      success = false;
    } else {
      sem->count++;
    }
  }
  clh_lock_release(&sem->lock);

  if (wakeup != NULL){
    scheduler_wake_thread(wakeup);
  }

  // Keep the operation live through scheduler publication. Destruction may
  // not free synchronization state while a detached waiter is in transit to
  // a ready queue.
  __atomic_fetch_add(&sem->active_operations, -1);
  return success;
}

// Publish one permit or wake one queued waiter.
void sem_up(struct Semaphore* sem){
  if (!sem_try_up(sem)) {
    int args[2] = {(int)sem, INT_MAX};
    say("| semaphore up rejected sem=0x%X count=%d\n", args);
    panic("sem_up: semaphore count overflow; use sem_try_up() for fallible input.\n");
  }
}

// Non-mutating teardown preflight. Composite objects call this for every
// embedded semaphore before destroying the first one, so a detectable live
// waiter cannot leave the composite only partly dismantled.
void sem_assert_destroyable(struct Semaphore* sem) {
  assert(sem != NULL, "sem_assert_destroyable: semaphore is NULL.\n");
  clh_lock_acquire(&sem->lock);

  int active = __atomic_load_n(&sem->active_operations);
  int waiters = sem->wait_queue.size;
  bool quiescent = active == 0 && waiters == 0 &&
    sem->wait_queue.head == NULL && sem->wait_queue.tail == NULL;

  clh_lock_release(&sem->lock);

  if (!quiescent) {
    int args[3] = {(int)sem, active, waiters};
    say("| semaphore destroy rejected sem=0x%X active=%d waiters=%d\n",
      args);
    panic("sem_destroy: owner must stop new operations and wake/join every waiter before destruction.\n");
  }
}

// Destroy only after the owner has made the semaphore externally quiescent.
//
// This function deliberately never moves a blocked TCB to reaper_queue. Only
// stop() may publish a terminal TCB and enqueue it for reaping; bypassing that
// path would leave child descriptors and wait_child() promises dangling.
void sem_destroy(struct Semaphore* sem) {
  assert(sem != NULL, "sem_destroy: semaphore is NULL.\n");
  sem_assert_destroyable(sem);

  /*
   * No operation that began before sem_destroy() can be waiting in the CLH
   * tail: every public operation increments active_operations before its first
   * lock exchange. The external owner contract forbids operations beginning
   * after destruction starts. Together those conditions make freeing the
   * final unlocked CLH tail node safe.
   */
  clh_lock_destroy(&sem->lock);
}

// Destroy a semaphore after its count and waiter queue are quiescent.
void sem_free(struct Semaphore* sem) {
  sem_destroy(sem);
  free(sem);
}
