#include "atomic.h"
#include "machine.h"
#include "threads.h"
#include "per_core.h"
#include "constants.h"
#include "debug.h"
#include "interrupts.h"
#include "threads.h"
#include "heap.h"

// return NULL until everything is set up to use tcb->my_node
static struct TCB* spin_lock_owner_tcb(void){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);
  if (tcb == NULL || tcb->my_node == NULL) {
    return NULL;
  }
  return tcb;
}

void spin_lock_init(struct SpinLock* lock){
  lock->the_lock = 0;
  lock->interrupt_state = 0;
}

// will disable interrupts on each attempt at getting the lock
// when it returns, interrupts are disabled
void spin_lock_acquire(struct SpinLock* lock){
  struct TCB* me = spin_lock_owner_tcb();

  
  assert(!me->my_node->locked,
    "spin_lock_acquire: thread attempted to acquire a spinlock while already holding one.\n");

  // wait until the value stored in the lock is 0
  while (true){
    int was = interrupts_disable();
    if (!__atomic_exchange_n(&lock->the_lock, 1)){
      // value was 0, now we have the lock
      __atomic_store_n(&lock->interrupt_state, was);
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
  int was = interrupts_disable();
  struct TCB* me = get_current_tcb();
  if (me->my_node->locked){
    // this thread already holds a spinlock, so fail
    interrupts_restore(was);
    return false;
  }

  if (!__atomic_exchange_n(&lock->the_lock, 1)){
    // value was 0, now we have the lock
    __atomic_store_n(&lock->interrupt_state, was);
    me->my_node->locked = true;
    return true;
  }
  interrupts_restore(was);

  return false;
}

// restores interrupt state
void spin_lock_release(struct SpinLock* lock){
  // interrupts already disabled, so this is safe
  struct TCB* me = get_current_tcb();
  assert(me->my_node->locked,
    "spin_lock_release: thread attempted to release a spinlock while not holding one.\n");
  me->my_node->locked = false;
  int was = __atomic_load_n(&lock->interrupt_state);
  __atomic_exchange_n(&lock->the_lock, 0);
  interrupts_restore(was);
}

// initializes a preempt spin lock to the unlocked state
void preempt_spin_lock_init(struct PreemptSpinLock* lock){
  lock->the_lock = 0;
  lock->preempt_state = false;
}

// will disable preemption on each attempt at getting the lock
// when it returns, preemption is disabled
void preempt_spin_lock_acquire(struct PreemptSpinLock* lock){
  // wait until the value stored in the lock is 0
  while (true){
    bool was = preemption_disable();
    if (!__atomic_exchange_n(&lock->the_lock, 1)){
      // Save the caller's prior preemption state so release restores callers
      // that were already non-preemptible before they acquired the lock.
      __atomic_store_n(&lock->preempt_state, was);
      return;
    }
    preemption_restore(was);
  }
}

// will disable preemption before attempting to get the lock
// if it succeeds, returns true with preemption disabled
// otherwise, returns false and restores preemption state
bool preempt_spin_lock_try_acquire(struct PreemptSpinLock* lock){
  bool was = preemption_disable();
  if (!__atomic_exchange_n(&lock->the_lock, 1)){
    // Save the caller's prior preemption state so release restores callers
    // that were already non-preemptible before they acquired the lock.
    __atomic_store_n(&lock->preempt_state, was);
    return true;
  }
  preemption_restore(was);
  return false;
}

// restores preemption state
void preempt_spin_lock_release(struct PreemptSpinLock* lock){
  bool was = __atomic_load_n(&lock->preempt_state);
  __atomic_exchange_n(&lock->the_lock, 0);
  preemption_restore(was);
}

// Initialize a CLH lock to the unlocked state
void clh_lock_init(struct CLHLock* lock){
  assert(lock != NULL, "clh_lock_init: lock is NULL.\n");
  lock->tail = malloc(sizeof(struct CLHNode));
  lock->tail->locked = false;
  lock->tail->interrupt_state = 0;
}

// Acquire a CLH lock in FIFO enqueue order
void clh_lock_acquire(struct CLHLock* lock){
  assert(lock != NULL, "clh_acquire: lock is NULL.\n");

  int was = interrupts_disable();
  struct TCB* me = get_current_tcb();

  assert(me != NULL, "clh_acquire: current TCB is NULL; CLH locks require thread context.\n");
  assert(me->my_node != NULL, "clh_acquire: control block node is NULL.\n");
  assert(me->my_pred == NULL, "clh_acquire: control block already owns or waits on a lock.\n");

  // mark our own node as locked
  __atomic_store_n(&me->my_node->locked, true);

  // swap ourselves into the tail of the queue
  struct CLHNode* pred = (struct CLHNode*)__atomic_exchange_n((int*)&lock->tail, (int)me->my_node);
  // record who is in front of us in the queue
  me->my_pred = pred;

  assert(pred != NULL, "clh_acquire: lock has NULL tail; was clh_lock_init() called?\n");

  // spin with interrupts disabled
  // this is fine because all critical sections are O(1),
  // and the fair spinlock ensure we wait for at most O(#cores = 4)
  while (true) {
    // spin until the thread in front of us releases the lock
    if (!__atomic_load_n(&pred->locked)){
      // pred node is done being used, so we can use it to store our interrupt state
      pred->interrupt_state = was;
      return;
    }
  }
}

// Release a CLH lock and hand ownership to the next queued waiter, if any
void clh_lock_release(struct CLHLock* lock){
  assert(lock != NULL, "clh_release: lock is NULL.\n");
  struct TCB* me = get_current_tcb(); // interrupts are disabled, so this is safe
  assert(me != NULL, "clh_release: current TCB is NULL; CLH locks require thread context.\n");
  assert(me->my_node->locked,
    "clh_release: thread attempted to release a CLH lock while not holding one.\n");

  __atomic_store_n(&me->my_node->locked, false);
  me->my_node = me->my_pred;
  me->my_pred = NULL;
  // we set my_pred->interrupt_state when we acquire the lock,
  // and just did my_node = my_pred
  interrupts_restore(me->my_node->interrupt_state);
}

void clh_lock_destroy(struct CLHLock* lock){
  assert(lock != NULL, "clh_lock_destroy: lock is NULL.\n");
  free(lock->tail);
  lock->tail = NULL;
}

void clh_lock_free(struct CLHLock* lock){
  clh_lock_destroy(lock);
  free(lock);
}

// simple barrier synchronization for a known number of threads
// threads spin until all threads have reached the barrier
void spin_barrier_sync(int* barrier){
  // ensure we are not holding a spinlock
  struct TCB* me = spin_lock_owner_tcb();
  assert(!me->my_node->locked,
    "spin_barrier_sync: thread attempted to synchronize while holding a spinlock.\n");

  __atomic_fetch_add(barrier, -1);
  while (__atomic_load_n(barrier) != 0);
}
