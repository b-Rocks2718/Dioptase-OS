#ifndef ATOMIC_H
#define ATOMIC_H

#include "constants.h"

// spin lock that disables interrupts while held
struct SpinLock {
  bool the_lock;
  int  interrupt_state;
};

// spin lock that disables preemption while held
struct PreemptSpinLock {
  bool the_lock;
  bool preempt_state;
};

// initializes a spin lock to the unlocked state
void spin_lock_init(struct SpinLock* lock);

// will disable interrupts on each attempt at getting the lock
// when it returns, interrupts are disabled
void spin_lock_acquire(struct SpinLock* lock);

// will disable interrupts before attempting to get the lock
// if it succeeds, returns true with interrupts disabled
// otherwise, returns false and restores interrupt state
bool spin_lock_try_acquire(struct SpinLock* lock);

// restores interrupt state
void spin_lock_release(struct SpinLock* lock);

// initializes a preempt spin lock to the unlocked state
void preempt_spin_lock_init(struct PreemptSpinLock* lock);

// will disable preemption on each attempt at getting the lock
// when it returns, preemption is disabled
void preempt_spin_lock_acquire(struct PreemptSpinLock* lock);

// will disable preemption before attempting to get the lock
// if it succeeds, returns true with preemption disabled
// otherwise, returns false and restores preemption state
bool preempt_spin_lock_try_acquire(struct PreemptSpinLock* lock);

// restores preemption state
void preempt_spin_lock_release(struct PreemptSpinLock* lock);

// simple barrier synchronization for a known number of threads
// threads spin until all threads have reached the barrier
void spin_barrier_sync(int* barrier);

#endif // ATOMIC_H
