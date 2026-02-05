#ifndef BLOCKING_LOCK_H
#define BLOCKING_LOCK_H

#include "semaphore.h"

// port of Gheith kernel implementation

struct BlockingLock {
  struct Semaphore semaphore;
  bool preempt;
};

void blocking_lock_init(struct BlockingLock* lock);

// Note: acquiring a blocking lock disables preemption
void blocking_lock_get(struct BlockingLock* lock);

// restores preemption state
void blocking_lock_release(struct BlockingLock* lock);

void blocking_lock_destroy(struct BlockingLock* lock);

void blocking_lock_free(struct BlockingLock* lock);

#endif // BLOCKING_LOCK_H