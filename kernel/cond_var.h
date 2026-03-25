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

// initialize an empty condition variable
void cond_var_init(struct CondVar* cv);

// release external_lock, block, and re-acquire it before returning
void cond_var_wait(struct CondVar* cv, struct BlockingLock* external_lock);

// wake exactly one waiter, if any.
void cond_var_signal(struct CondVar* cv, struct BlockingLock* external_lock);

// wake all current waiters.
void cond_var_broadcast(struct CondVar* cv, struct BlockingLock* external_lock);

// destroy the condition variable and reap any waiters
void cond_var_destroy(struct CondVar* cv);

// destroy the condition variable and free its memory
void cond_var_free(struct CondVar* cv);

#endif // COND_VAR_H
