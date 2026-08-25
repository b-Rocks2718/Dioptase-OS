#ifndef BLOCKING_LOCK_H
#define BLOCKING_LOCK_H

#include "semaphore.h"

struct TCB;

// mutex-style lock built from a semaphore
struct BlockingLock {
  struct Semaphore semaphore;
  bool preempt; // caller's preemption state from the successful acquire
  bool is_held; // debugging flag for misuse detection
  // Exact mutex owner. Published after sem_down succeeds and cleared before
  // sem_up hands the permit to another thread.
  struct TCB* owner;
};

// initialize lock in unlocked state
void blocking_lock_init(struct BlockingLock* lock);

// block until lock is acquired
// acquiring a blocking lock disables preemption
void blocking_lock_acquire(struct BlockingLock* lock);

// release lock and restore preemption state
void blocking_lock_release(struct BlockingLock* lock);

// Destroy synchronization state after the owner has prevented new operations,
// released the holder, and woken/joined every waiter.
void blocking_lock_destroy(struct BlockingLock* lock);

// Destroy a quiescent lock and free it.
void blocking_lock_free(struct BlockingLock* lock);

#endif // BLOCKING_LOCK_H
