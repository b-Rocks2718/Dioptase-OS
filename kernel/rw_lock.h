#ifndef RW_LOCK_H
#define RW_LOCK_H

#include "semaphore.h"
#include "queue.h"

// Reader-Writer Lock
// Allows multiple readers or one writer at a time.
// Write-preferring implementation: readers block if any writer is waiting.
struct RwLock {
  struct SpinLock lock;
  struct Queue waiting_readers;
  struct Queue waiting_writers;
  unsigned readers; // invariant: readers > 0 implies !writer_active
  bool writer_active;
};

// initialize an empty write-preferring reader-writer lock
void rw_lock_init(struct RwLock* rwlock);

// acquire shared access; blocks behind active or queued writers
void rw_lock_acquire_read(struct RwLock* rwlock);

// release shared access
void rw_lock_release_read(struct RwLock* rwlock);

// acquire exclusive access
void rw_lock_acquire_write(struct RwLock* rwlock);

// release exclusive access and hand off to queued writers or readers
void rw_lock_release_write(struct RwLock* rwlock);

// destroy the lock and reap queued readers and writers
void rw_lock_destroy(struct RwLock* rwlock);

// destroy the lock and free its memory
void rw_lock_free(struct RwLock* rwlock);

#endif // RW_LOCK_H
