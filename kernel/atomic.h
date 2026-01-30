#ifndef ATOMIC_H
#define ATOMIC_H

extern void spin_lock_get(int* lock);

extern void spin_lock_release(int* lock);

extern int* make_spin_barrier(int count);

extern void spin_barrier_sync(int* barrier);

#endif // ATOMIC_H
