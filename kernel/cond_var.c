#include "cond_var.h"
#include "heap.h"
#include "debug.h"

// Condition variable implementation with semaphore-backed wakeups.

void cond_var_init(struct CondVar* cv){
  sem_init(&cv->semaphore, 0);
  spin_lock_init(&cv->lock);
  cv->waiters = 0;
}

// Contract note:
// The caller must wrap this call in a predicate loop:
//   while (!predicate) cond_var_wait(cv, lock);
void cond_var_wait(struct CondVar* cv, struct BlockingLock* external_lock){
  assert(cv != NULL, "cond_var wait: cv is NULL.\n");
  assert(external_lock != NULL, "cond_var wait: external lock is NULL.\n");
  assert(external_lock->is_held, "cond_var wait: external lock must be held by caller.\n");

  // increment waiters count
  spin_lock_acquire(&cv->lock);
  cv->waiters += 1;
  spin_lock_release(&cv->lock);

  // release external lock before waiting
  blocking_lock_release(external_lock);

  // wait on the condition variable's semaphore
  // this will block the thread until signaled
  sem_down(&cv->semaphore);

  // re-acquire the external lock after being signaled
  blocking_lock_acquire(external_lock);
}

void cond_var_signal(struct CondVar* cv, struct BlockingLock* external_lock){
  assert(cv != NULL, "cond_var signal: cv is NULL.\n");
  assert(external_lock != NULL, "cond_var signal: external lock is NULL.\n");
  assert(external_lock->is_held, "cond_var signal: external lock must be held by caller.\n");

  bool should_wake = false;

  spin_lock_acquire(&cv->lock);
  if (cv->waiters > 0) {
    cv->waiters -= 1;
    should_wake = true;
  }
  spin_lock_release(&cv->lock);

  if (should_wake) {
    sem_up(&cv->semaphore);
  }
}

void cond_var_broadcast(struct CondVar* cv, struct BlockingLock* external_lock){
  assert(cv != NULL, "cond_var broadcast: cv is NULL.\n");
  assert(external_lock != NULL, "cond_var broadcast: external lock is NULL.\n");
  assert(external_lock->is_held, "cond_var broadcast: external lock must be held by caller.\n");

  spin_lock_acquire(&cv->lock);
  unsigned n = cv->waiters;
  cv->waiters = 0;
  spin_lock_release(&cv->lock);

  // signal all waiting threads
  for (unsigned i = 0; i < n; i++) {
    sem_up(&cv->semaphore);
  }
}

void cond_var_destroy(struct CondVar* cv){
  assert(cv != NULL, "cond_var destroy: cv is NULL.\n");
  sem_destroy(&cv->semaphore);
}

void cond_var_free(struct CondVar* cv){
  assert(cv != NULL, "cond_var free: cv is NULL.\n");
  cond_var_destroy(cv);
  free(cv);
}
