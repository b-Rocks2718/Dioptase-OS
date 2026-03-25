#ifndef BLOCKING_LOCK_H
#define BLOCKING_LOCK_H

#include "semaphore.h"

// mutex-style lock built from a semaphore
struct BlockingLock {
  struct Semaphore semaphore;
  bool preempt; // caller's preemption state from the successful acquire
  bool is_held; // debugging flag for misuse detection
};

// initialize lock in unlocked state
void blocking_lock_init(struct BlockingLock* lock);

// block until lock is acquired
// acquiring a blocking lock disables preemption
void blocking_lock_acquire(struct BlockingLock* lock);

// release lock and restore preemption state
void blocking_lock_release(struct BlockingLock* lock);

// destroy lock and free any resources it holds, but not the lock itself
// waiting threads will be reaped
void blocking_lock_destroy(struct BlockingLock* lock);

// free lock and its resources
// waiting threads will be reaped
void blocking_lock_free(struct BlockingLock* lock);

#endif // BLOCKING_LOCK_H
