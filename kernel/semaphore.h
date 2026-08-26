#ifndef SEMAPHORE_H
#define SEMAPHORE_H

#include "atomic.h"
#include "queue.h"

// Semaphore allows only a set number of threads to access a resource at once.
//
// Lifecycle contract:
// - The owner must prevent new operations, wake/join every waiter, and ensure
//   every sem_down/up/try_down call has returned before destruction.
// - Destruction never kills or reaps a waiter. A blocked TCB must resume and
//   eventually terminate through the normal thread-exit publication path.
// - active_operations covers the entire public operation, including the gap
//   between sem_down's first count check and its block() enqueue callback.
//   It is diagnostic state protected by sequentially-consistent atomic RMWs;
//   it is not a substitute for the owner's external lifetime guarantee.
struct Semaphore {
  struct CLHLock lock;
  int count;
  struct Queue wait_queue;
  int active_operations;
};

// Initialize with a non-negative signed count. INT_MAX is valid; a later
// sem_try_up reports that it cannot grow further.
void sem_init(struct Semaphore* sem, int initial_count);

// decrement the semaphore count, or block if the count is 0 until another thread calls sem_up
void sem_down(struct Semaphore* sem);

// attempt to decrement the semaphore count without blocking
// returns true if a permit was consumed, false if the count was 0
bool sem_try_down(struct Semaphore* sem);

// Wake one waiter or increment the count. Kernel/composite callers use this
// invariant-enforcing form; it panics if an unconsumed count would overflow.
void sem_up(struct Semaphore* sem);

// Wake one waiter or, if no waiter exists, increment without signed overflow.
// Returns false only when count is already INT_MAX and no waiter can accept the
// permit. Public syscall paths use this fallible form to report invalid input.
bool sem_try_up(struct Semaphore* sem);

// Verify, without destroying state, that no operation or waiter is live.
// Composite primitives use this to preflight every embedded semaphore before
// dismantling any part of the enclosing object. The same external-quiescence
// precondition as sem_destroy applies.
void sem_assert_destroyable(struct Semaphore* sem);

// Destroy a quiescent semaphore. Panics with an actionable diagnostic if an
// operation or waiter is still live; the caller retains ownership on failure.
void sem_destroy(struct Semaphore* sem);

// Destroy a quiescent semaphore and free its memory.
void sem_free(struct Semaphore* sem);

#endif // SEMAPHORE_H
