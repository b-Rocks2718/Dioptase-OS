#ifndef BARRIER_H
#define BARRIER_H

#include "semaphore.h"

// port of Gheith kernel

// Note: destroyed barrier reaps all waiting threads.
struct Barrier {
  unsigned count;
  struct Semaphore sem;
};

void barrier_init(struct Barrier* barrier, unsigned count);

void barrier_sync(struct Barrier* barrier);

void barrier_destroy(struct Barrier* barrier);

void barrier_free(struct Barrier* barrier);

#endif // BARRIER_H