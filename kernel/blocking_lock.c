#include "blocking_lock.h"
#include "semaphore.h"
#include "heap.h"
#include "debug.h"
#include "threads.h"
#include "print.h"

/*
  Lock implementation is a semaphore(1),
  plus disabling/restoring preemption on acquire/release
*/

// initialize lock in unlocked state
void blocking_lock_init(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock init: lock is NULL.\n");
  sem_init(&lock->semaphore, 1);
}

// block until lock is acquired
// acquiring a blocking lock disables preemption
void blocking_lock_acquire(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock acquire: lock is NULL.\n");
  bool was_preempt = preemption_disable();
  sem_down(&lock->semaphore);
  // Save the caller's preemption state only after this thread actually owns the
  // lock. Waiting threads must not overwrite the holder's saved state.
  lock->preempt = was_preempt;
  lock->is_held = true;
}

// release lock and restore preemption state
void blocking_lock_release(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock release: lock is NULL.\n");
  if (!lock->is_held){
    int args[1] = {(int)lock};
    printf_uart("blocking lock at %X is not currently held.\n", args);
  }
  assert(lock->is_held, "blocking lock release: lock is not currently held.\n");
  // Snapshot the holder's saved state before waking the next waiter. Another
  // core may acquire the lock immediately after sem_up() and replace
  // lock->preempt with its own state.
  bool was_preempt = lock->preempt;
  lock->is_held = false;
  sem_up(&lock->semaphore);
  preemption_restore(was_preempt);
}

// destroy lock and free any resources it holds, but not the lock itself
// waiting threads will be reaped
void blocking_lock_destroy(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock destroy: lock is NULL.\n");
  sem_destroy(&lock->semaphore);
}

// free lock and its resources
// waiting threads will be reaped
void blocking_lock_free(struct BlockingLock* lock){
  blocking_lock_destroy(lock);
  free(lock);
}
