#ifndef SEMAPHORE_H
#define SEMAPHORE_H

// port of Gheith kernel implementation

#include "atomic.h"
#include "queue.h"

// Semaphore allows only a set number of threads to access a resource at once
// Note: destroyed semaphore will reap all waiting threads.
struct Semaphore {
  struct SpinLock lock;
  int count;
  struct Queue wait_queue;
};

// initialize a semaphore with the given count
void sem_init(struct Semaphore* sem, int initial_count);

// decrement the semaphore count, or block if the count is 0 until another thread calls sem_up
void sem_down(struct Semaphore* sem);

// attempt to decrement the semaphore count without blocking
// returns true if a permit was consumed, false if the count was 0
bool sem_try_down(struct Semaphore* sem);

// waking one waiting thread if any are waiting, or incrementing the count if not
void sem_up(struct Semaphore* sem);

// destroy the semaphore and reap all waiting threads
void sem_destroy(struct Semaphore* sem);

// destroy the semaphore and free its memory. Reaps all waiting threads
void sem_free(struct Semaphore* sem);

#endif // SEMAPHORE_H
