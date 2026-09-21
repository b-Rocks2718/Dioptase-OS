#include "atomic.h"
#include "machine.h"
#include "threads.h"
#include "per_core.h"
#include "constants.h"
#include "debug.h"
#include "interrupts.h"
#include "heap.h"

#define LOCK_NO_OWNER_CORE (-1)

// Initialize an interrupt-masking spin lock without an owner.
void spin_lock_init(struct SpinLock* lock){
  assert(lock != NULL, "spin_lock_init: lock is NULL.\n");
  lock->the_lock = 0;
  lock->interrupt_state = 0;
  lock->owner = NULL;
}

// will disable interrupts on each attempt at getting the lock
// when it returns, interrupts are disabled
void spin_lock_acquire(struct SpinLock* lock){
  assert(lock != NULL, "spin_lock_acquire: lock is NULL.\n");
  int was = interrupts_disable();
  struct TCB* me = get_current_tcb();
  interrupts_restore(was);
  
  assert_always(!me->my_node->locked,
    "spin_lock_acquire: thread attempted to acquire a spinlock while already holding one.\n");

  // wait until the value stored in the lock is 0
  while (true){
    int was = interrupts_disable();
    if (!__atomic_exchange_n(&lock->the_lock, 1)){
      // value was 0, now we have the lock
      assert_always(lock->owner == NULL,
        "spin_lock_acquire: unlocked spinlock retained an owner.\n");
      __atomic_store_n(&lock->interrupt_state, was);
      __atomic_store_n((int*)&lock->owner, (int)me);
      me->my_node->locked = true;
      return;
    }
    interrupts_restore(was);
  }
}

// will disable interrupts before attempting to get the lock
// if it succeeds, returns true with interrupts disabled
// otherwise, returns false and restores interrupt state
bool spin_lock_try_acquire(struct SpinLock* lock){
  assert(lock != NULL, "spin_lock_try_acquire: lock is NULL.\n");
  int was = interrupts_disable();
  struct TCB* me = get_current_tcb();
  if (me->my_node->locked){
    // this thread already holds a spinlock, so fail
    interrupts_restore(was);
    return false;
  }

  if (!__atomic_exchange_n(&lock->the_lock, 1)){
    // value was 0, now we have the lock
    assert_always(lock->owner == NULL,
      "spin_lock_try_acquire: unlocked spinlock retained an owner.\n");
    __atomic_store_n(&lock->interrupt_state, was);
    __atomic_store_n((int*)&lock->owner, (int)me);
    me->my_node->locked = true;
    return true;
  }
  interrupts_restore(was);

  return false;
}

// restores interrupt state
void spin_lock_release(struct SpinLock* lock){
  assert(lock != NULL, "spin_lock_release: lock is NULL.\n");
  // interrupts already disabled, so this is safe
  struct TCB* me = get_current_tcb();
  assert_always(me->my_node->locked,
    "spin_lock_release: thread attempted to release a spinlock while not holding one.\n");
  assert_always(lock->the_lock && lock->owner == me,
    "spin_lock_release: current thread does not own the requested spinlock.\n");
  me->my_node->locked = false;
  int was = __atomic_load_n(&lock->interrupt_state);
  __atomic_store_n((int*)&lock->owner, (int)NULL);
  __atomic_exchange_n(&lock->the_lock, 0);
  interrupts_restore(was);
}

// initializes a preempt spin lock to the unlocked state
void preempt_spin_lock_init(struct PreemptSpinLock* lock){
  assert(lock != NULL, "preempt_spin_lock_init: lock is NULL.\n");
  lock->the_lock = 0;
  lock->preempt_state = false;
  lock->owner_core = LOCK_NO_OWNER_CORE;
}

// will disable preemption on each attempt at getting the lock
// when it returns, preemption is disabled
void preempt_spin_lock_acquire(struct PreemptSpinLock* lock){
  assert(lock != NULL, "preempt_spin_lock_acquire: lock is NULL.\n");
  // wait until the value stored in the lock is 0
  while (true){
    bool was = preemption_disable();
    if (!__atomic_exchange_n(&lock->the_lock, 1)){
      // Save the caller's prior preemption state so release restores callers
      // that were already non-preemptible before they acquired the lock.
      __atomic_store_n(&lock->preempt_state, was);
      __atomic_store_n(&lock->owner_core, get_core_id());
      return;
    }
    preemption_restore(was);
  }
}

// will disable preemption before attempting to get the lock
// if it succeeds, returns true with preemption disabled
// otherwise, returns false and restores preemption state
bool preempt_spin_lock_try_acquire(struct PreemptSpinLock* lock){
  assert(lock != NULL, "preempt_spin_lock_try_acquire: lock is NULL.\n");
  bool was = preemption_disable();
  if (!__atomic_exchange_n(&lock->the_lock, 1)){
    // Save the caller's prior preemption state so release restores callers
    // that were already non-preemptible before they acquired the lock.
    __atomic_store_n(&lock->preempt_state, was);
    __atomic_store_n(&lock->owner_core, get_core_id());
    return true;
  }
  preemption_restore(was);
  return false;
}

// restores preemption state
void preempt_spin_lock_release(struct PreemptSpinLock* lock){
  assert(lock != NULL, "preempt_spin_lock_release: lock is NULL.\n");
  assert_always(lock->the_lock && __atomic_load_n(&lock->owner_core) == get_core_id(),
    "preempt_spin_lock_release: current core does not own the requested lock.\n");
  bool was = __atomic_load_n(&lock->preempt_state);
  __atomic_store_n(&lock->owner_core, LOCK_NO_OWNER_CORE);
  __atomic_exchange_n(&lock->the_lock, 0);
  preemption_restore(was);
}

// Initialize a CLH lock to the unlocked state
void clh_lock_init(struct CLHLock* lock){
  assert(lock != NULL, "clh_lock_init: lock is NULL.\n");
  lock->tail = malloc(sizeof(struct CLHNode));
  lock->tail->locked = false;
  lock->tail->interrupt_state = 0;
  lock->owner = NULL;
}

// Acquire a CLH lock in FIFO enqueue order
void clh_lock_acquire(struct CLHLock* lock){
  assert(lock != NULL, "clh_acquire: lock is NULL.\n");

  int was = interrupts_disable();
  struct TCB* me = get_current_tcb();

  assert_always(me != NULL, "clh_acquire: current TCB is NULL; CLH locks require thread context.\n");
  assert_always(me->my_node != NULL, "clh_acquire: control block node is NULL.\n");
  assert_always(me->my_pred == NULL, "clh_acquire: control block already owns or waits on a lock.\n");
  assert_always(!me->my_node->locked, "clh_acquire: thread attempted to acquire a spinlock while already holding one.\n");

  // mark our own node as locked
  __atomic_store_n(&me->my_node->locked, true);

  // swap ourselves into the tail of the queue
  struct CLHNode* pred = (struct CLHNode*)__atomic_exchange_n((int*)&lock->tail, (int)me->my_node);
  // record who is in front of us in the queue
  me->my_pred = pred;

  assert_always(pred != NULL, "clh_acquire: lock has NULL tail; was clh_lock_init() called?\n");

  // spin with interrupts disabled
  // this is fine because all critical sections are O(1),
  // and the fair spinlock ensure we wait for at most O(#cores = 4)
  while (true) {
    // spin until the thread in front of us releases the lock
    if (!__atomic_load_n(&pred->locked)){
      // pred node is done being used, so we can use it to store our interrupt state
      assert_always(lock->owner == NULL,
        "clh_acquire: unlocked CLH lock retained an owner.\n");
      pred->interrupt_state = was;
      __atomic_store_n((int*)&lock->owner, (int)me);
      return;
    }
  }
}

// Release a CLH lock and hand ownership to the next queued waiter, if any
void clh_lock_release(struct CLHLock* lock){
  assert(lock != NULL, "clh_release: lock is NULL.\n");
  struct TCB* me = get_current_tcb(); // interrupts are disabled, so this is safe
  assert_always(me != NULL, "clh_release: current TCB is NULL; CLH locks require thread context.\n");
  assert_always(me->my_node->locked,
    "clh_release: thread attempted to release a CLH lock while not holding one.\n");
  assert_always(me->my_pred != NULL, "clh_release: thread attempted to release a CLH lock with no recorded predecessor.\n");
  assert_always(lock->owner == me,
    "clh_release: current thread does not own the requested CLH lock.\n");

  __atomic_store_n((int*)&lock->owner, (int)NULL);
  __atomic_store_n(&me->my_node->locked, false);
  me->my_node = me->my_pred;
  me->my_pred = NULL;
  // we set my_pred->interrupt_state when we acquire the lock,
  // and just did my_node = my_pred
  interrupts_restore(me->my_node->interrupt_state);
}

// Destroy an idle CLH lock and release its sentinel node.
void clh_lock_destroy(struct CLHLock* lock){
  assert(lock != NULL, "clh_lock_destroy: lock is NULL.\n");
  assert_always(lock->owner == NULL,
    "clh_lock_destroy: lock still has an owner.\n");
  assert_always(lock->tail != NULL && !lock->tail->locked,
    "clh_lock_destroy: lock tail is active; stop and join every user first.\n");
  free(lock->tail);
  lock->tail = NULL;
}

// Destroy a heap-allocated CLH lock after all users have stopped.
void clh_lock_free(struct CLHLock* lock){
  clh_lock_destroy(lock);
  free(lock);
}

// simple barrier synchronization for a known number of threads
// threads spin until all threads have reached the barrier
void spin_barrier_sync(int* barrier){
  // ensure we are not holding a spinlock
  int was = interrupts_disable();
  struct TCB* me = get_current_tcb();
  interrupts_restore(was);
  assert(!me->my_node->locked,
    "spin_barrier_sync: thread attempted to synchronize while holding a spinlock.\n");

  __atomic_fetch_add(barrier, -1);
  while (__atomic_load_n(barrier) != 0);
}
