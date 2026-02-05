#ifndef SEMAPHORE_H
#define SEMAPHORE_H

// port of Gheith kernel implementation

#include "atomic.h"
#include "queue.h"

// Note: destroyed semaphore will reap all waiting threads.
struct Semaphore {
  struct SpinLock lock;
  int count;
  struct Queue wait_queue;  
};

void sem_init(struct Semaphore* sem, int initial_count);

void sem_down(struct Semaphore* sem);

void sem_up(struct Semaphore* sem);

void sem_destroy(struct Semaphore* sem);

void sem_free(struct Semaphore* sem);

#endif // SEMAPHORE_H
