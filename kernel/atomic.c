#include "atomic.h"
#include "machine.h"
#include "threads.h"
#include "per_core.h"
#include "constants.h"
#include "debug.h"
#include "interrupts.h"

void spin_lock_init(struct SpinLock* lock){
  lock->the_lock = 0;
  lock->interrupt_state = 0;
}

// will disable interrupt on each attempt at getting the lock
// when it returns, interrupts are disabled
void spin_lock_get(struct SpinLock* lock){

  // wait until the value stored in the lock is 0
  while (true){
    int was = disable_interrupts();
    if (!__atomic_exchange_n(&lock->the_lock, 1)){
      // value was 0, now we have the lock
      __atomic_store_n(&lock->interrupt_state, was);
      return;
    }
    restore_interrupts(was);
    pause();
  }
}

// will disable interrupt before attempting to get the lock
// if it succeeds, interrupts are disabled
bool spin_lock_try_get(struct SpinLock* lock){

  int was = disable_interrupts();
  if (!__atomic_exchange_n(&lock->the_lock, 1)){
    // value was 0, now we have the lock
    __atomic_store_n(&lock->interrupt_state, was);
    return true;
  }
  restore_interrupts(was);

  return false;
}

// restores interrupt state
void spin_lock_release(struct SpinLock* lock){
  int was = __atomic_load_n(&lock->interrupt_state);
  __atomic_exchange_n(&lock->the_lock, 0);
  restore_interrupts(was);
}

void spin_barrier_sync(int* barrier){
  __atomic_fetch_add(barrier, -1);
  while (__atomic_load_n(barrier) != 0);
}
