#ifndef BARRIER_H
#define BARRIER_H

#include "semaphore.h"

struct Barrier {
  unsigned count;
  struct Semaphore sem;
};

void barrier_init(struct Barrier* barrier, unsigned count);

void barrier_sync(struct Barrier* barrier);

#endif // BARRIER_H