#include "blocking_lock.h"
#include "semaphore.h"
#include "heap.h"
#include "debug.h"

void blocking_lock_init(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock init: lock is NULL.\n");
  lock->semaphore = malloc(sizeof(struct Semaphore));
  sem_init(lock->semaphore, 1);
}

void blocking_lock_get(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock get: lock is NULL.\n");
  assert(lock->semaphore != NULL, "blocking lock get: semaphore is NULL.\n");
  sem_down(lock->semaphore);
}

void blocking_lock_release(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock release: lock is NULL.\n");
  assert(lock->semaphore != NULL, "blocking lock release: semaphore is NULL.\n");
  sem_up(lock->semaphore);
}

void blocking_lock_destroy(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock destroy: lock is NULL.\n");
  if (lock->semaphore != NULL) {
    free(lock->semaphore);
    lock->semaphore = NULL;
  }
}
