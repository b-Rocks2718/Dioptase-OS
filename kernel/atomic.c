#include "atomic.h"
#include "machine.h"

void spin_lock_get(int* lock){
  // wait until the value stored in the lock is 0
  while (__atomic_exchange_n(lock, 1));

  // value was 0, now we have the lock
}

void spin_lock_release(int* lock){
  __atomic_exchange_n(lock, 0);
}

void spin_barrier_sync(int* barrier){
  __atomic_fetch_add(barrier, -1);
  while (__atomic_load_n(barrier) != 0);
}
