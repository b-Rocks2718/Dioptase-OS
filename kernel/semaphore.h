#ifndef SEMAPHORE_H
#define SEMAPHORE_H

#include "atomic.h"
#include "queue.h"

struct Semaphore {
  struct SpinLock lock;
  int count;
  struct Queue wait_queue;  
};

void sem_init(struct Semaphore* sem, int initial_count);

void sem_down(struct Semaphore* sem);

void sem_up(struct Semaphore* sem);

#endif // SEMAPHORE_H
