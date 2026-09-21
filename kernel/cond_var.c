#include "cond_var.h"
#include "semaphore.h"
#include "heap.h"
#include "debug.h"
#include "print.h"

// Each waiter owns a private semaphore and links itself into cv->wait_queue
// before releasing the external lock. Signal/broadcast remove concrete waiter
// nodes from that queue and wake those exact waiters. This prevents a future
// waiter from stealing a wakeup that belonged to an already-queued waiter.

struct CondVarWaiter {
  struct GenericQueueElement link;
  struct Semaphore semaphore;
};

// Initialize an empty condition-variable waiter queue.
void cond_var_init(struct CondVar* cv){
  clh_lock_init(&cv->lock);
  generic_queue_init(&cv->wait_queue);
  cv->waiters = 0;
  __atomic_store_n(&cv->active_operations, 0);
}

// Contract note:
// The caller must wrap this call in a predicate loop:
//   while (!predicate) cond_var_wait(cv, lock);
void cond_var_wait(struct CondVar* cv, struct BlockingLock* external_lock){
  assert(cv != NULL, "cond_var wait: cv is NULL.\n");
  assert(external_lock != NULL, "cond_var wait: external lock is NULL.\n");
  assert(external_lock->is_held, "cond_var wait: external lock must be held by caller.\n");

  /*
   * Keep this operation live from before the stack-owned waiter semaphore is
   * initialized until after it is destroyed. This covers both the interval
   * before publication and the interval after signal removes the waiter but
   * before this continuation re-acquires external_lock.
   *
   * Preconditions: external_lock protects the predicate, and the CondVar
   * owner has not begun destruction. The owner keeps both objects allocated.
   * Postcondition: on return this waiter is unlinked, holds external_lock, and
   * no stack-owned semaphore remains reachable from another core.
   */
  __atomic_fetch_add(&cv->active_operations, 1);

  struct CondVarWaiter waiter;
  sem_init(&waiter.semaphore, 0);

  // Publish this waiter before releasing the external lock so any later
  // signal/broadcast can target this exact waiter, even if it has not reached
  // sem_down() yet.
  clh_lock_acquire(&cv->lock);
  generic_queue_add(&cv->wait_queue, &waiter.link);
  cv->waiters += 1;
  clh_lock_release(&cv->lock);

  // release external lock before waiting
  blocking_lock_release(external_lock);

  // Wait on this waiter's private semaphore. If signal/broadcast ran after the
  // waiter was published but before sem_down(), the semaphore count will
  // already be positive and this returns immediately.
  sem_down(&waiter.semaphore);

  // re-acquire the external lock after being signaled
  blocking_lock_acquire(external_lock);

  sem_destroy(&waiter.semaphore);
  __atomic_fetch_add(&cv->active_operations, -1);
}

// Wake one waiter while the caller holds the associated external lock.
void cond_var_signal(struct CondVar* cv, struct BlockingLock* external_lock){
  assert(cv != NULL, "cond_var signal: cv is NULL.\n");
  assert(external_lock != NULL, "cond_var signal: external lock is NULL.\n");
  assert(external_lock->is_held, "cond_var signal: external lock must be held by caller.\n");

  __atomic_fetch_add(&cv->active_operations, 1);
  struct CondVarWaiter* waiter = NULL;

  clh_lock_acquire(&cv->lock);
  if (cv->waiters > 0) {
    waiter = (struct CondVarWaiter*)generic_queue_remove(&cv->wait_queue);
    assert(waiter != NULL,
      "cond_var signal: waiter count was non-zero but queue was empty.\n");
    cv->waiters -= 1;
  }
  clh_lock_release(&cv->lock);

  if (waiter != NULL) {
    sem_up(&waiter->semaphore);
  }
  __atomic_fetch_add(&cv->active_operations, -1);
}

// Wake every waiter while the caller holds the associated external lock.
void cond_var_broadcast(struct CondVar* cv, struct BlockingLock* external_lock){
  assert(cv != NULL, "cond_var broadcast: cv is NULL.\n");
  assert(external_lock != NULL, "cond_var broadcast: external lock is NULL.\n");
  assert(external_lock->is_held, "cond_var broadcast: external lock must be held by caller.\n");

  __atomic_fetch_add(&cv->active_operations, 1);
  struct CondVarWaiter* waiter = NULL;

  clh_lock_acquire(&cv->lock);
  waiter = (struct CondVarWaiter*)generic_queue_remove_all(&cv->wait_queue);
  cv->waiters = 0;
  clh_lock_release(&cv->lock);

  while (waiter != NULL) {
    struct CondVarWaiter* next = (struct CondVarWaiter*)waiter->link.next;
    waiter->link.next = NULL;
    sem_up(&waiter->semaphore);
    waiter = next;
  }
  __atomic_fetch_add(&cv->active_operations, -1);
}

// Destroy the waiter queue after all waiters have left.
void cond_var_destroy(struct CondVar* cv){
  assert(cv != NULL, "cond_var destroy: cv is NULL.\n");

  clh_lock_acquire(&cv->lock);
  int active = __atomic_load_n(&cv->active_operations);
  int waiters = cv->waiters;
  bool quiescent = active == 0 && waiters == 0 &&
    cv->wait_queue.size == 0 && cv->wait_queue.head == NULL &&
    cv->wait_queue.tail == NULL;
  clh_lock_release(&cv->lock);

  if (!quiescent) {
    int args[3] = {(int)cv, active, waiters};
    say("| cond_var destroy rejected cv=0x%X active=%d waiters=%d\n",
      args);
    panic("cond_var_destroy: owner must stop new operations and wake/join every waiter before destruction.\n");
  }

  // active_operations begins before any public operation can exchange itself
  // into this CLH tail. The external no-new-operation guarantee therefore
  // makes the final unlocked tail node safe to free.
  clh_lock_destroy(&cv->lock);
}

// Destroy and free a heap-allocated condition variable.
void cond_var_free(struct CondVar* cv){
  assert(cv != NULL, "cond_var free: cv is NULL.\n");
  cond_var_destroy(cv);
  free(cv);
}
