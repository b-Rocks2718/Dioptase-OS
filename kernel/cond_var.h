#ifndef COND_VAR_H
#define COND_VAR_H

#include "semaphore.h"
#include "atomic.h"
#include "queue.h"
#include "blocking_lock.h"

// Condition variable.
//
// Contract:
// - A CondVar is associated with exactly one predicate and one external
//   BlockingLock that protects that predicate.
// - Callers must hold that external lock before calling cond_var_wait,
//   cond_var_signal, or cond_var_broadcast.
// - cond_var_wait releases external_lock before blocking and re-acquires it
//   before returning.
// - signal/broadcast must be performed while holding external_lock and after
//   updating the predicate state, otherwise wakeup ordering is undefined.
// - Waiters must always check their predicate in a loop around cond_var_wait.
//
// Notes:
// - This implementation does not track or validate which external lock is
//   paired with a CondVar; callers must keep usage consistent.
struct CondVar {
  struct Semaphore semaphore;
  struct SpinLock lock;
  unsigned waiters;
};

// Purpose: initialize a condition variable.
void cond_var_init(struct CondVar* cv);

// Purpose: block until signaled/broadcast while atomically dropping/re-taking
// the caller's external predicate lock.
// Inputs: cv is the condition variable; external_lock protects the predicate.
// Preconditions: caller holds external_lock; cv is initialized; kernel mode.
// Postconditions: caller holds external_lock again when this returns.
void cond_var_wait(struct CondVar* cv, struct BlockingLock* external_lock);

// Purpose: wake exactly one waiter, if any.
// Inputs: cv is the condition variable.
// Preconditions: caller holds the external predicate lock paired with cv.
// Postconditions: one waiting thread becomes runnable if waiters > 0.
void cond_var_signal(struct CondVar* cv);

// Purpose: wake all current waiters.
// Inputs: cv is the condition variable.
// Preconditions: caller holds the external predicate lock paired with cv.
// Postconditions: all current waiters become runnable.
void cond_var_broadcast(struct CondVar* cv);

#endif // COND_VAR_H
