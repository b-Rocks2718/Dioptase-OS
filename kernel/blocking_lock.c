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

void blocking_lock_acquire(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock acquire: lock is NULL.\n");
  bool was_preempt = disable_preemption();
  sem_down(&lock->semaphore);
  // Save the caller's preemption state only after this thread actually owns the
  // lock. Waiting threads must not overwrite the holder's saved state.
  lock->preempt = was_preempt;
}

void blocking_lock_release(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock release: lock is NULL.\n");
  // Snapshot the holder's saved state before waking the next waiter. Another
  // core may acquire the lock immediately after sem_up() and replace
  // lock->preempt with its own state.
  bool was_preempt = lock->preempt;
  sem_up(&lock->semaphore);
  enable_preemption(was_preempt);
}

void blocking_lock_destroy(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock destroy: lock is NULL.\n");
  sem_destroy(&lock->semaphore);
}

void blocking_lock_free(struct BlockingLock* lock){
  blocking_lock_destroy(lock);
  free(lock);
}
