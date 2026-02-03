#ifndef BLOCKING_LOCK_H
#define BLOCKING_LOCK_H

#include "semaphore.h"

struct BlockingLock {
  struct Semaphore* semaphore;
};

void blocking_lock_init(struct BlockingLock* lock);

void blocking_lock_get(struct BlockingLock* lock);

void blocking_lock_release(struct BlockingLock* lock);

void blocking_lock_destroy(struct BlockingLock* lock);

#endif // BLOCKING_LOCK_H