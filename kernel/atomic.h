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

// CLH Node for fair spin lock
// each thread gets exactly one CLH Node, as part of their TCB
// Normal spinlock still marks the CLHNode as locked,
// to enforce the invariant that threads only ever acquire one spinlock at a time
// PreemptSpinLock is exempt from this invariant, but should only be used
// for kernel debug printing
struct CLHNode {
  bool locked;
  int interrupt_state;
};

// CLH lock for fair spin lock
struct CLHLock {
  struct CLHNode* tail;
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

// initializes a CLH lock to the unlocked state
void clh_lock_init(struct CLHLock* lock);

// will acquire the CLH lock in a fair manner, with FIFO ordering
void clh_lock_acquire(struct CLHLock* lock);

// releases the CLH lock, allowing the next waiting thread to acquire it
void clh_lock_release(struct CLHLock* lock);

// destroys a CLH lock after it is done being used
void clh_lock_destroy(struct CLHLock* lock);

// destroys a CLH lock and frees its memory after it is no longer needed
void clh_lock_free(struct CLHLock* lock);

// simple barrier synchronization for a known number of threads
// threads spin until all threads have reached the barrier
void spin_barrier_sync(int* barrier);

// Atomically set *ptr to val, and return the old value of *ptr
extern int __atomic_exchange_n(int *ptr, int val);

// Atomically add val to *ptr, and return the old value of *ptr
extern int __atomic_fetch_add(int* ptr, int val);

// Atomically load value stored in *ptr
extern int __atomic_load_n(int* ptr);

// Atomically store val into *ptr
extern void __atomic_store_n(int* ptr, int val);

#endif // ATOMIC_H
