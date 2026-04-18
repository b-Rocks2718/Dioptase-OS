#ifndef ATOMIC_H
#define ATOMIC_H

#include "constants.h"

// spin lock that disables interrupts while held
struct SpinLock {
  bool the_lock;
};

// initializes a spin lock to the unlocked state
void spin_lock_init(struct SpinLock* lock);

void spin_lock_acquire(struct SpinLock* lock);

bool spin_lock_try_acquire(struct SpinLock* lock);

void spin_lock_release(struct SpinLock* lock);

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