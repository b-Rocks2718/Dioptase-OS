#include "rw_lock.h"
#include "threads.h"
#include "interrupts.h"
#include "per_core.h"
#include "debug.h"
#include "heap.h"
#include "scheduler.h"
#include "print.h"

// Reader-writer lock implementation (write-preferring).
// Waiting readers and writers are queued; writers are granted priority when present.

void rw_lock_init(struct RwLock* rwlock){
  clh_lock_init(&rwlock->lock);
  queue_init(&rwlock->waiting_readers);
  queue_init(&rwlock->waiting_writers);
  rwlock->readers = 0;
  rwlock->writer_active = false;
  __atomic_store_n(&rwlock->active_operations, 0);
}

// block() callback for readers: either claim a read slot or enqueue
static void rw_add_reader(void* arg){
  int** args = (int**)arg;
  struct RwLock* rwlock = (struct RwLock*)args[0];
  struct TCB* tcb = (struct TCB*)args[1];

  clh_lock_acquire(&rwlock->lock);

  if (!rwlock->writer_active && rwlock->waiting_writers.size == 0){
    rwlock->readers++;
    clh_lock_release(&rwlock->lock);
    scheduler_wake_thread(tcb);
  } else {
    queue_add(&rwlock->waiting_readers, tcb);
    clh_lock_release(&rwlock->lock);
  }
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
  clh_lock_acquire(&rwlock->lock);

  if (!rwlock->writer_active && rwlock->waiting_writers.size == 0){
    // No active writer and no waiting writers, can acquire read lock
    rwlock->readers++;
    clh_lock_release(&rwlock->lock);
    __atomic_fetch_add(&rwlock->active_operations, -1);
    return;
  }

  clh_lock_release(&rwlock->lock);

  int was = interrupts_disable();

  struct TCB* current_tcb = get_current_tcb();

  int* args[2] = { (int*)rwlock, (int*)current_tcb };
  block(was, (void (*)(void *))rw_add_reader, (void*)(args), true);
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

// block() callback for writers: either claim write ownership or enqueue
static void rw_add_writer(void* arg){
  int** args = (int**)arg;
  struct RwLock* rwlock = (struct RwLock*)args[0];
  struct TCB* tcb = (struct TCB*)args[1];

  clh_lock_acquire(&rwlock->lock);

  // check if nobody else has the lock
  if (!rwlock->writer_active && rwlock->readers == 0){
    // take the lock
    rwlock->writer_active = true;
    clh_lock_release(&rwlock->lock);
    scheduler_wake_thread(tcb);
  } else {
    // wait in the writers queue
    queue_add(&rwlock->waiting_writers, tcb);
    clh_lock_release(&rwlock->lock);
  }
}

// Acquire exclusive ownership after all existing readers and writers leave.
void rw_lock_acquire_write(struct RwLock* rwlock){
  assert(rwlock != NULL, "rw_lock_acquire_write: lock is NULL.\n");
  __atomic_fetch_add(&rwlock->active_operations, 1);
  clh_lock_acquire(&rwlock->lock);

  if (!rwlock->writer_active && rwlock->readers == 0){
    // No active writer and no active readers, can acquire write lock
    rwlock->writer_active = true;
    clh_lock_release(&rwlock->lock);
    __atomic_fetch_add(&rwlock->active_operations, -1);
    return;
  }

  clh_lock_release(&rwlock->lock);

  int was = interrupts_disable();

  struct TCB* current_tcb = get_current_tcb();

  int* args[2] = { (int*)rwlock, (int*)current_tcb };
  block(was, (void (*)(void *))rw_add_writer, (void*)(args), true);
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
