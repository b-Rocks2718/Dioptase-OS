#include "barrier.h"
#include "semaphore.h"
#include "machine.h"
#include "debug.h"
#include "heap.h"

// port of Gheith kernel implementation

void barrier_init(struct Barrier* barrier, unsigned count) {
  assert(barrier != NULL, "barrier init: barrier is NULL.\n");
  assert(count > 0, "barrier init: count must be > 0.\n");
  barrier->count = count;
  sem_init(&barrier->sem, 0);
}


void barrier_sync(struct Barrier* barrier) {
  assert(barrier != NULL, "barrier sync: barrier is NULL.\n");
  if (__atomic_fetch_add((int*)&barrier->count, -1) == 1) {
    sem_up(&barrier->sem);
  } else {
    sem_down(&barrier->sem);
    sem_up(&barrier->sem);
  }
}

void barrier_destroy(struct Barrier* barrier) {
  assert(barrier != NULL, "barrier destroy: barrier is NULL.\n");
  sem_destroy(&barrier->sem);
}

void barrier_free(struct Barrier* barrier) {
  barrier_destroy(barrier);
  free(barrier);
}
