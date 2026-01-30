#ifndef ATOMIC_H
#define ATOMIC_H

#include "constants.h"

struct SpinLock {
  bool the_lock;
  int  interrupt_state;
};

// will disable interrupt on each attempt at getting the lock
// when it returns, interrupts are disabled
extern void spin_lock_get(struct SpinLock* lock);

// restores interrupt state
extern void spin_lock_release(struct SpinLock* lock);

extern int* make_spin_barrier(int count);

extern void spin_barrier_sync(int* barrier);

#endif // ATOMIC_H
