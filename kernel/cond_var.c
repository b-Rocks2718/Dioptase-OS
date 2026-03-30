#include "cond_var.h"
#include "semaphore.h"
#include "heap.h"
#include "debug.h"

// Each waiter owns a private semaphore and links itself into cv->wait_queue
// before releasing the external lock. Signal/broadcast remove concrete waiter
// nodes from that queue and wake those exact waiters. This prevents a future
// waiter from stealing a wakeup that belonged to an already-queued waiter.

struct CondVarWaiter {
  struct GenericQueueElement* link;
  struct Semaphore semaphore;
};

void cond_var_init(struct CondVar* cv){
  spin_lock_init(&cv->lock);
  generic_queue_init(&cv->wait_queue);
  cv->waiters = 0;
}

// Contract note:
// The caller must wrap this call in a predicate loop:
//   while (!predicate) cond_var_wait(cv, lock);
void cond_var_wait(struct CondVar* cv, struct BlockingLock* external_lock){
  assert(cv != NULL, "cond_var wait: cv is NULL.\n");
  assert(external_lock != NULL, "cond_var wait: external lock is NULL.\n");
  assert(external_lock->is_held, "cond_var wait: external lock must be held by caller.\n");

  struct CondVarWaiter waiter;
  sem_init(&waiter.semaphore, 0);

  // Publish this waiter before releasing the external lock so any later
  // signal/broadcast can target this exact waiter, even if it has not reached
  // sem_down() yet.
  spin_lock_acquire(&cv->lock);
  generic_queue_add(&cv->wait_queue, waiter.link);
  cv->waiters += 1;
  spin_lock_release(&cv->lock);

  // release external lock before waiting
  blocking_lock_release(external_lock);

  // Wait on this waiter's private semaphore. If signal/broadcast ran after the
  // waiter was published but before sem_down(), the semaphore count will
  // already be positive and this returns immediately.
  sem_down(&waiter.semaphore);

  // re-acquire the external lock after being signaled
  blocking_lock_acquire(external_lock);

  sem_destroy(&waiter.semaphore);
}

void cond_var_signal(struct CondVar* cv, struct BlockingLock* external_lock){
  assert(cv != NULL, "cond_var signal: cv is NULL.\n");
  assert(external_lock != NULL, "cond_var signal: external lock is NULL.\n");
  assert(external_lock->is_held, "cond_var signal: external lock must be held by caller.\n");

  struct CondVarWaiter* waiter = NULL;

  spin_lock_acquire(&cv->lock);
  if (cv->waiters > 0) {
    waiter = (struct CondVarWaiter*)generic_queue_remove(&cv->wait_queue);
    assert(waiter != NULL,
      "cond_var signal: waiter count was non-zero but queue was empty.\n");
    cv->waiters -= 1;
  }
  spin_lock_release(&cv->lock);

  if (waiter != NULL) {
    sem_up(&waiter->semaphore);
  }
}

void cond_var_broadcast(struct CondVar* cv, struct BlockingLock* external_lock){
  assert(cv != NULL, "cond_var broadcast: cv is NULL.\n");
  assert(external_lock != NULL, "cond_var broadcast: external lock is NULL.\n");
  assert(external_lock->is_held, "cond_var broadcast: external lock must be held by caller.\n");

  struct CondVarWaiter* waiter = NULL;

  spin_lock_acquire(&cv->lock);
  waiter = (struct CondVarWaiter*)generic_queue_remove_all(&cv->wait_queue);
  cv->waiters = 0;
  spin_lock_release(&cv->lock);

  while (waiter != NULL) {
    struct CondVarWaiter* next = (struct CondVarWaiter*)waiter->link->next;
    waiter->link->next = NULL;
    sem_up(&waiter->semaphore);
    waiter = next;
  }
}

void cond_var_destroy(struct CondVar* cv){
  assert(cv != NULL, "cond_var destroy: cv is NULL.\n");

  spin_lock_acquire(&cv->lock);
  struct CondVarWaiter* waiter =
    (struct CondVarWaiter*)generic_queue_remove_all(&cv->wait_queue);
  cv->waiters = 0;
  spin_lock_release(&cv->lock);

  while (waiter != NULL) {
    struct CondVarWaiter* next = (struct CondVarWaiter*)waiter->link->next;
    waiter->link->next = NULL;
    sem_destroy(&waiter->semaphore);
    waiter = next;
  }
}

void cond_var_free(struct CondVar* cv){
  assert(cv != NULL, "cond_var free: cv is NULL.\n");
  cond_var_destroy(cv);
  free(cv);
}
