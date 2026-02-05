#include "blocking_lock.h"
#include "semaphore.h"
#include "heap.h"
#include "debug.h"
#include "threads.h"

// port of Gheith kernel implementation

void blocking_lock_init(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock init: lock is NULL.\n");
  sem_init(&lock->semaphore, 1);
}

void blocking_lock_get(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock get: lock is NULL.\n");
  lock->preempt = disable_preemption();
  sem_down(&lock->semaphore);
}

void blocking_lock_release(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock release: lock is NULL.\n");
  sem_up(&lock->semaphore);
  enable_preemption(lock->preempt);
}

void blocking_lock_destroy(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock destroy: lock is NULL.\n");
  sem_destroy(&lock->semaphore);
}

void blocking_lock_free(struct BlockingLock* lock){
  blocking_lock_destroy(lock);
  free(lock);
}
