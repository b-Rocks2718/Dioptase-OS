#include "blocking_lock.h"
#include "semaphore.h"
#include "heap.h"
#include "debug.h"

void blocking_lock_init(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock init: lock is NULL.\n");
  sem_init(&lock->semaphore, 1);
}

void blocking_lock_get(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock get: lock is NULL.\n");
  sem_down(&lock->semaphore);
}

void blocking_lock_release(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock release: lock is NULL.\n");
  sem_up(&lock->semaphore);
}
