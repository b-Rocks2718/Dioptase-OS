#include "atomic.h"

#include "constants.h"
#include "debug.h"
#include "sys.h"

void spin_lock_init(struct SpinLock* lock){
  lock->the_lock = 0;
}

// will disable interrupts on each attempt at getting the lock
// when it returns, interrupts are disabled
void spin_lock_acquire(struct SpinLock* lock){

  // wait until the value stored in the lock is 0
  while (true){
    if (!__atomic_exchange_n(&lock->the_lock, 1)){
      // value was 0, now we have the lock
      return;
    }
    yield();
  }
}

// will disable interrupts before attempting to get the lock
// if it succeeds, returns true with interrupts disabled
// otherwise, returns false and restores interrupt state
bool spin_lock_try_acquire(struct SpinLock* lock){
  if (!__atomic_exchange_n(&lock->the_lock, 1)){
    // value was 0, now we have the lock
    return true;
  }

  return false;
}

// restores interrupt state
void spin_lock_release(struct SpinLock* lock){
  __atomic_exchange_n(&lock->the_lock, 0);
}

// simple barrier synchronization for a known number of threads
// threads spin until all threads have reached the barrier
void spin_barrier_sync(int* barrier){
  __atomic_fetch_add(barrier, -1);
  while (__atomic_load_n(barrier) != 0);
}
