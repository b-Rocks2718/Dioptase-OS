#ifndef BARRIER_H
#define BARRIER_H

#include "semaphore.h"
#include "blocking_lock.h"

// Reusable barrier for synchronizing a fixed number of threads
// Note: barrier must be reused by the same set of threads, otherwise behavior is undefined.

// Note: destroyed barrier reaps all waiting threads.
struct Barrier {
  unsigned initial_count;
  unsigned current_count;
  struct BlockingLock lock;
  struct Semaphore sem_1;
  struct Semaphore sem_2;
};

void barrier_init(struct Barrier* barrier, unsigned count);

void barrier_sync(struct Barrier* barrier);

void barrier_destroy(struct Barrier* barrier);

void barrier_free(struct Barrier* barrier);

#endif // BARRIER_H