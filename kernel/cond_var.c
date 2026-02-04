#include "cond_var.h"

// Condition variable implementation with semaphore-backed wakeups.
// The caller-provided external BlockingLock protects the predicate state.

void cond_var_init(struct CondVar* cv){
  sem_init(&cv->semaphore, 0);
  spin_lock_init(&cv->lock);
  cv->waiters = 0;
}

// Purpose: wait on a condition while releasing/reacquiring the predicate lock.
// Inputs: cv is the condition variable; external_lock protects the predicate.
// Preconditions: caller holds external_lock; cv is initialized; kernel mode.
// Postconditions: caller holds external_lock again when function returns.
//
// Contract note:
// The caller must wrap this call in a predicate loop:
//   while (!predicate) cond_var_wait(cv, lock);
void cond_var_wait(struct CondVar* cv, struct BlockingLock* external_lock){
  // increment waiters count
  spin_lock_get(&cv->lock);
  cv->waiters += 1;
  spin_lock_release(&cv->lock);

  // release external lock before waiting
  blocking_lock_release(external_lock);

  // wait on the condition variable's semaphore
  // this will block the thread until signaled
  sem_down(&cv->semaphore);

  // re-acquire the external lock after being signaled
  blocking_lock_get(external_lock);
}

// Purpose: wake one waiter if any are currently blocked.
// Inputs: cv is the condition variable.
// Preconditions: caller holds the external predicate lock associated with cv.
// Postconditions: at most one waiter is released.
void cond_var_signal(struct CondVar* cv){
  spin_lock_get(&cv->lock);
  if (cv->waiters > 0) {
    cv->waiters -= 1;
    // signal one waiting thread
    sem_up(&cv->semaphore);
  }
  spin_lock_release(&cv->lock);
}

// Purpose: wake all current waiters.
// Inputs: cv is the condition variable.
// Preconditions: caller holds the external predicate lock associated with cv.
// Postconditions: all waiters present at function entry are released.
void cond_var_broadcast(struct CondVar* cv){
  spin_lock_get(&cv->lock);
  unsigned n = cv->waiters;
  cv->waiters = 0;
  spin_lock_release(&cv->lock);

  // signal all waiting threads
  for (unsigned i = 0; i < n; i++) {
    sem_up(&cv->semaphore);
  }
}
