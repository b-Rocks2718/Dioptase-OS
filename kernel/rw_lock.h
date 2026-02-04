#ifndef RW_LOCK_H
#define RW_LOCK_H

#include "semaphore.h"
#include "queue.h"

// Reader-Writer Lock
// Allows multiple readers or one writer at a time.
// Write-preferring implementation: readers block if any writer is waiting.
// Invariants:
// - readers > 0 implies writer_active == false.
// - writer_active == true implies readers == 0 and either a writer holds
//   the lock or has been granted it and is about to run.
struct RwLock {
  struct SpinLock lock;
  struct Queue waiting_readers;
  struct Queue waiting_writers;
  unsigned readers; // invariant: readers > 0 implies !writer_active
  bool writer_active;
};

void rw_lock_init(struct RwLock* rwlock);

void rw_lock_acquire_read(struct RwLock* rwlock);

void rw_lock_release_read(struct RwLock* rwlock);

void rw_lock_acquire_write(struct RwLock* rwlock);

void rw_lock_release_write(struct RwLock* rwlock);

#endif // RW_LOCK_H
