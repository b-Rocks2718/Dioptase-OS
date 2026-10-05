#include "rw_lock.h"
#include "threads.h"
#include "debug.h"
#include "heap.h"
#include "scheduler.h"
#include "print.h"

// Reader-writer lock implementation (write-preferring).
// Waiting readers and writers are queued; writers are granted priority when present.

// Initialize an unlocked reader-writer lock with empty wait queues.
void rw_lock_init(struct RwLock* rwlock){
  clh_lock_init(&rwlock->lock);
  queue_init(&rwlock->waiting_readers);
  queue_init(&rwlock->waiting_writers);
  rwlock->readers = 0;
  rwlock->writer_active = false;
  __atomic_store_n(&rwlock->active_operations, 0);
}

// Claim a read slot unless a writer is active or waiting (write preference).
// Caller holds rwlock->lock.
static bool rw_try_read(void* arg){
  struct RwLock* rwlock = (struct RwLock*)arg;
  if (!rwlock->writer_active && rwlock->waiting_writers.size == 0){
    rwlock->readers++;
    return true;
  }
  return false;
}

// Claim exclusive ownership if no reader or writer holds the lock.
// Caller holds rwlock->lock.
static bool rw_try_write(void* arg){
  struct RwLock* rwlock = (struct RwLock*)arg;
  if (!rwlock->writer_active && rwlock->readers == 0){
    rwlock->writer_active = true;
    return true;
  }
  return false;
}

// Acquire shared ownership, blocking behind an active or waiting writer.
void rw_lock_acquire_read(struct RwLock* rwlock){
  assert(rwlock != NULL, "rw_lock_acquire_read: lock is NULL.\n");

  /*
   * Begin the operation before touching the CLH tail. This count remains live
   * across block(), covering both queued readers and the pre-enqueue context-
   * switch interval. The owner must keep rwlock allocated until this call
   * returns and must prevent new calls before destruction.
   */
  __atomic_fetch_add(&rwlock->active_operations, 1);
  acquire_or_block(&rwlock->lock, &rwlock->waiting_readers, rw_try_read,
    rwlock);
  __atomic_fetch_add(&rwlock->active_operations, -1);
}

// Release shared ownership and wake the next writer when the last reader leaves.
void rw_lock_release_read(struct RwLock* rwlock){
  assert(rwlock != NULL, "rw_lock_release_read: lock is NULL.\n");
  __atomic_fetch_add(&rwlock->active_operations, 1);
  clh_lock_acquire(&rwlock->lock);

  assert_always(rwlock->readers > 0, "rw_lock_release_read: no active readers\n");

  rwlock->readers--;

  // check if there's no waiting readers and there are waiting writers
  if (rwlock->readers == 0 && rwlock->waiting_writers.size > 0){
    // Wake up one waiting writer
    struct TCB* writer = queue_remove(&rwlock->waiting_writers);
    rwlock->writer_active = true;
    clh_lock_release(&rwlock->lock);
    scheduler_wake_thread(writer);
  } else {
    clh_lock_release(&rwlock->lock);
  }

  // Include ready-queue publication in the operation lifetime so destruction
  // cannot race a detached writer between this lock and the scheduler.
  __atomic_fetch_add(&rwlock->active_operations, -1);
}

// Acquire exclusive ownership after all existing readers and writers leave.
void rw_lock_acquire_write(struct RwLock* rwlock){
  assert(rwlock != NULL, "rw_lock_acquire_write: lock is NULL.\n");
  __atomic_fetch_add(&rwlock->active_operations, 1);
  acquire_or_block(&rwlock->lock, &rwlock->waiting_writers, rw_try_write,
    rwlock);
  __atomic_fetch_add(&rwlock->active_operations, -1);
}

// Release exclusive ownership, preferring the next queued writer.
void rw_lock_release_write(struct RwLock* rwlock){
  assert(rwlock != NULL, "rw_lock_release_write: lock is NULL.\n");
  __atomic_fetch_add(&rwlock->active_operations, 1);
  clh_lock_acquire(&rwlock->lock);

  assert_always(rwlock->writer_active, "rw_lock_release_write: no active writer\n");

  rwlock->writer_active = false;

  if (rwlock->waiting_writers.size > 0){
    // Wake up one waiting writer
    struct TCB* writer = queue_remove(&rwlock->waiting_writers);
    rwlock->writer_active = true;
    clh_lock_release(&rwlock->lock);
    scheduler_wake_thread(writer);
  } else {
    // Wake up all waiting readers.
    // Count and claim reader slots while holding the lock so writers cannot slip in.
    unsigned wake_count = rwlock->waiting_readers.size;
    struct TCB* readers = queue_remove_all(&rwlock->waiting_readers);
    rwlock->readers += wake_count;
    clh_lock_release(&rwlock->lock);

    while (readers != NULL){
      struct TCB* next = readers->next;
      readers->next = NULL;
      scheduler_wake_thread(readers);
      readers = next;
    }
  }

  // No detached reader/writer remains in transit once this reaches zero.
  __atomic_fetch_add(&rwlock->active_operations, -1);
}

// Destroy an idle reader-writer lock after all holders and waiters leave.
void rw_lock_destroy(struct RwLock* rwlock) {
  assert(rwlock != NULL, "rw_lock_destroy: lock is NULL.\n");
  clh_lock_acquire(&rwlock->lock);

  int active = __atomic_load_n(&rwlock->active_operations);
  int readers = rwlock->readers;
  int writers = rwlock->writer_active ? 1 : 0;
  int waiting_readers = rwlock->waiting_readers.size;
  int waiting_writers = rwlock->waiting_writers.size;
  bool quiescent = active == 0 && readers == 0 && writers == 0 &&
    waiting_readers == 0 && waiting_writers == 0;
  clh_lock_release(&rwlock->lock);

  if (!quiescent) {
    int args[6] = {
      (int)rwlock, active, readers, writers, waiting_readers, waiting_writers
    };
    say("| rw_lock destroy rejected lock=0x%X active=%d readers=%d writer=%d waiting_readers=%d waiting_writers=%d\n",
      args);
    panic("rw_lock_destroy: owner must stop new operations, release holders, and wake/join waiters before destruction.\n");
  }

  // active_operations starts before every public operation's CLH exchange.
  // With a zero snapshot and the external no-new-operation guarantee, no TCB
  // can own or wait behind the tail node that clh_lock_destroy() now frees.
  clh_lock_destroy(&rwlock->lock);
}

// Destroy and free a heap-allocated reader-writer lock.
void rw_lock_free(struct RwLock* rwlock){
  rw_lock_destroy(rwlock);
  free(rwlock);
}
