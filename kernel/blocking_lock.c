#include "blocking_lock.h"
#include "semaphore.h"
#include "heap.h"
#include "debug.h"
#include "threads.h"
#include "per_core.h"
#include "print.h"

/*
  Lock implementation is a semaphore(1),
  plus disabling/restoring preemption on acquire/release
*/

// initialize lock in unlocked state
void blocking_lock_init(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock init: lock is NULL.\n");
  sem_init(&lock->semaphore, 1);
  lock->preempt = false;
  lock->is_held = false;
  lock->owner = NULL;
}

// block until lock is acquired
// acquiring a blocking lock disables preemption
void blocking_lock_acquire(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock acquire: lock is NULL.\n");
  bool was_preempt = preemption_disable();
  sem_down(&lock->semaphore);

  struct TCB* me = get_current_tcb();
  assert_always(me != NULL,
    "blocking lock acquire: current TCB is NULL after semaphore acquisition.\n");
  assert_always(!lock->is_held && lock->owner == NULL,
    "blocking lock acquire: semaphore granted a lock that retained an owner.\n");

  // Save the caller's preemption state only after this thread actually owns the
  // lock. Waiting threads must not overwrite the holder's saved state.
  lock->preempt = was_preempt;
  __atomic_store_n((int*)&lock->owner, (int)me);
  lock->is_held = true;
}

// release lock and restore preemption state
void blocking_lock_release(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock release: lock is NULL.\n"); 
  assert_always(lock->is_held, "blocking lock release: lock is not currently held.\n");
  struct TCB* me = get_current_tcb();
  assert_always(me != NULL,
    "blocking lock release: current TCB is NULL.\n");
  assert_always(lock->owner == me,
    "blocking lock release: current thread does not own this lock.\n");
  // Snapshot the holder's saved state before waking the next waiter. Another
  // core may acquire the lock immediately after sem_up() and replace
  // lock->preempt with its own state.
  bool was_preempt = lock->preempt;
  lock->is_held = false;
  __atomic_store_n((int*)&lock->owner, (int)NULL);
  sem_up(&lock->semaphore);
  preemption_restore(was_preempt);
}

// Destroy lock synchronization state, but not the lock object itself.
// Preconditions: the owner has stopped new acquisitions, every holder has
// released, and every blocked acquisition has been woken and joined.
void blocking_lock_destroy(struct BlockingLock* lock){
  assert(lock != NULL, "blocking lock destroy: lock is NULL.\n");
  assert_always(!lock->is_held,
    "blocking_lock_destroy: lock is held; owner must release and join all users before destruction.\n");
  assert_always(lock->owner == NULL,
    "blocking_lock_destroy: lock retained an owner after its held flag cleared.\n");
  sem_destroy(&lock->semaphore);
}

// Free a lock only after the same externally quiescent destruction contract.
void blocking_lock_free(struct BlockingLock* lock){
  blocking_lock_destroy(lock);
  free(lock);
}
