#include "barrier.h"
#include "semaphore.h"
#include "machine.h"
#include "debug.h"
#include "heap.h"

// reusable two-turnstile barrier built from a blocking lock and two semaphores

// initialize the barrier with the given count of threads
void barrier_init(struct Barrier* barrier, unsigned count) {
  assert(barrier != NULL, "barrier init: barrier is NULL.\n");
  assert(count > 0, "barrier init: count must be > 0.\n");
  barrier->initial_count = count;
  barrier->current_count = count;

  blocking_lock_init(&barrier->lock);
  sem_init(&barrier->sem_1, 0); // closed
  sem_init(&barrier->sem_2, 1); // open
}

// block until the given count of threads have called barrier_sync()
// last call to sync() allows all threads to continue, and then resets the barrier
void barrier_sync(struct Barrier* barrier) {
  assert(barrier != NULL, "barrier sync: barrier is NULL.\n");

  // Phase 1: arrival
  blocking_lock_acquire(&barrier->lock);
  barrier->current_count--;
  if (barrier->current_count == 0) {
    // last thread has arrived, so we can open the barrier for all threads to depart
    // because last thread has arrived, we know all threads have left the previous generation
    // therefore we can close the second gate
    sem_down(&barrier->sem_2);   // close second gate for this generation
    sem_up(&barrier->sem_1);     // open first gate
  }
  blocking_lock_release(&barrier->lock);

  sem_down(&barrier->sem_1);
  sem_up(&barrier->sem_1);

  // Phase 2: departure
  blocking_lock_acquire(&barrier->lock);
  barrier->current_count++;
  if (barrier->current_count == barrier->initial_count) {
    // last thread has departed, so we can close the barrier for all threads to arrive for the next generation
    // because last thread has departed, we know all threads have arrived for this generation
    // therefore we can open the second gate for the next generation
    sem_down(&barrier->sem_1);   // close first gate for next generation
    sem_up(&barrier->sem_2);     // open second gate

    // the barrier is now reset for the next generation
  }
  blocking_lock_release(&barrier->lock);

  sem_down(&barrier->sem_2);
  sem_up(&barrier->sem_2);
}

// free resources used by the barrier, but does not free the barrier struct itself
// waiting threads will be reaped
void barrier_destroy(struct Barrier* barrier) {
  assert(barrier != NULL, "barrier destroy: barrier is NULL.\n");
  sem_destroy(&barrier->sem_1);
  sem_destroy(&barrier->sem_2);
}

// free the barrier struct and all resources used by the barrier
// waiting threads will be reaped
void barrier_free(struct Barrier* barrier) {
  barrier_destroy(barrier);
  free(barrier);
}
