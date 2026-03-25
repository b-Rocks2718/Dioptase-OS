#ifndef BARRIER_H
#define BARRIER_H

#include "semaphore.h"
#include "blocking_lock.h"

// Reusable barrier for synchronizing a fixed number of threads
// Note: barrier must be reused by the same set of threads, otherwise behavior is undefined.
struct Barrier {
  unsigned initial_count;
  unsigned current_count;
  struct BlockingLock lock;
  struct Semaphore sem_1;
  struct Semaphore sem_2;
};

// initialize the barrier with the given count of threads
void barrier_init(struct Barrier* barrier, unsigned count);

// block until the given count of threads have called barrier_sync()
// last call to sync() allows all threads to continue, and then resets the barrier
void barrier_sync(struct Barrier* barrier);

// free resources used by the barrier, but does not free the barrier struct itself
// waiting threads will be reaped
void barrier_destroy(struct Barrier* barrier);

// free the barrier struct and all resources used by the barrier
// waiting threads will be reaped
void barrier_free(struct Barrier* barrier);

#endif // BARRIER_H