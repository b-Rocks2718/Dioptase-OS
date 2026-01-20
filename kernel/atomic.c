#include "atomic.h"
#include "machine.h"

void get_spinlock(int* lock){
  // wait until the value stored in the lock is 0
  while (__atomic_exchange_n(lock, 1));

  // value was 0, now we have the lock
}

void release_spinlock(int* lock){
  __atomic_exchange_n(lock, 0);
}
