#include "atomic.h"
#include "machine.h"
#include "threads.h"
#include "per_core.h"
#include "constants.h"
#include "debug.h"
#include "interrupts.h"
#include "threads.h"

void spin_lock_init(struct SpinLock* lock){
  lock->the_lock = 0;
  lock->interrupt_state = 0;
}

// will disable interrupts on each attempt at getting the lock
// when it returns, interrupts are disabled
void spin_lock_acquire(struct SpinLock* lock){

  // wait until the value stored in the lock is 0
  while (true){
    int was = interrupts_disable();
    if (!__atomic_exchange_n(&lock->the_lock, 1)){
      // value was 0, now we have the lock
      __atomic_store_n(&lock->interrupt_state, was);
      return;
    }
    interrupts_restore(was);
    pause();
  }
}

// will disable interrupts before attempting to get the lock
// if it succeeds, returns true with interrupts disabled
// otherwise, returns false and restores interrupt state
bool spin_lock_try_acquire(struct SpinLock* lock){

  int was = interrupts_disable();
  if (!__atomic_exchange_n(&lock->the_lock, 1)){
    // value was 0, now we have the lock
    __atomic_store_n(&lock->interrupt_state, was);
    return true;
  }
  interrupts_restore(was);

  return false;
}

// restores interrupt state
void spin_lock_release(struct SpinLock* lock){
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
    pause();
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

// simple barrier synchronization for a known number of threads
// threads spin until all threads have reached the barrier
void spin_barrier_sync(int* barrier){
  __atomic_fetch_add(barrier, -1);
  while (__atomic_load_n(barrier) != 0);
}
