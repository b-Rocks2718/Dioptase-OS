#ifndef ATOMIC_H
#define ATOMIC_H

#include "constants.h"

struct SpinLock {
  bool the_lock;
  int  interrupt_state;
};

void spin_lock_init(struct SpinLock* lock);

// will disable interrupt on each attempt at getting the lock
// when it returns, interrupts are disabled
void spin_lock_get(struct SpinLock* lock);

bool spin_lock_try_get(struct SpinLock* lock);

// restores interrupt state
void spin_lock_release(struct SpinLock* lock);

void spin_barrier_sync(int* barrier);

#endif // ATOMIC_H
