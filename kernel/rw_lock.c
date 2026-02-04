#include "rw_lock.h"
#include "threads.h"
#include "interrupts.h"
#include "per_core.h"
#include "debug.h"

// Reader-writer lock implementation (write-preferring).
// Waiting readers and writers are queued; writers are granted priority when present.

void rw_lock_init(struct RwLock* rwlock){
  spin_lock_init(&rwlock->lock);
  queue_init(&rwlock->waiting_readers);
  queue_init(&rwlock->waiting_writers);
  rwlock->readers = 0;
  rwlock->writer_active = false;
}

static void rw_add_reader(void* arg){
  int** args = (int**)arg;
  struct RwLock* rwlock = (struct RwLock*)args[0];
  struct TCB* tcb = (struct TCB*)args[1];

  spin_lock_acquire(&rwlock->lock);

  if (!rwlock->writer_active && rwlock->waiting_writers.size == 0){
    rwlock->readers++;
    spin_lock_release(&rwlock->lock);
    spin_queue_add(&ready_queue, tcb);
  } else {
    queue_add(&rwlock->waiting_readers, tcb);
    spin_lock_release(&rwlock->lock);
  }
}

void rw_lock_acquire_read(struct RwLock* rwlock){
  spin_lock_acquire(&rwlock->lock);

  if (!rwlock->writer_active && rwlock->waiting_writers.size == 0){
    // No active writer and no waiting writers, can acquire read lock
    rwlock->readers++;
    spin_lock_release(&rwlock->lock);
    return;
  }

  spin_lock_release(&rwlock->lock);

  int was = disable_interrupts();

  struct TCB* current_tcb = get_current_tcb();

  int* args[2] = { (int*)rwlock, (int*)current_tcb };
  block(was, (void (*)(void *))rw_add_reader, (void*)(args));
}

void rw_lock_release_read(struct RwLock* rwlock){
  spin_lock_acquire(&rwlock->lock);

  assert(rwlock->readers > 0, "rw_lock_release_read: no active readers\n");

  rwlock->readers--;

  if (rwlock->readers == 0 && rwlock->waiting_writers.size > 0){
    // Wake up one waiting writer
    struct TCB* writer = queue_remove(&rwlock->waiting_writers);
    rwlock->writer_active = true;
    spin_lock_release(&rwlock->lock);
    spin_queue_add(&ready_queue, writer);
  } else {
    spin_lock_release(&rwlock->lock);
  }
}

static void rw_add_writer(void* arg){
  int** args = (int**)arg;
  struct RwLock* rwlock = (struct RwLock*)args[0];
  struct TCB* tcb = (struct TCB*)args[1];

  spin_lock_acquire(&rwlock->lock);

  if (!rwlock->writer_active && rwlock->readers == 0){
    rwlock->writer_active = true;
    spin_lock_release(&rwlock->lock);
    spin_queue_add(&ready_queue, tcb);
  } else {
    queue_add(&rwlock->waiting_writers, tcb);
    spin_lock_release(&rwlock->lock);
  }
}

void rw_lock_acquire_write(struct RwLock* rwlock){
  spin_lock_acquire(&rwlock->lock);

  if (!rwlock->writer_active && rwlock->readers == 0){
    // No active writer and no active readers, can acquire write lock
    rwlock->writer_active = true;
    spin_lock_release(&rwlock->lock);
    return;
  }

  spin_lock_release(&rwlock->lock);

  int was = disable_interrupts();

  struct TCB* current_tcb = get_current_tcb();

  int* args[2] = { (int*)rwlock, (int*)current_tcb };
  block(was, (void (*)(void *))rw_add_writer, (void*)(args));
}

void rw_lock_release_write(struct RwLock* rwlock){
  spin_lock_acquire(&rwlock->lock);

  assert(rwlock->writer_active, "rw_lock_release_write: no active writer\n");

  rwlock->writer_active = false;

  if (rwlock->waiting_writers.size > 0){
    // Wake up one waiting writer
    struct TCB* writer = queue_remove(&rwlock->waiting_writers);
    rwlock->writer_active = true;
    spin_lock_release(&rwlock->lock);
    spin_queue_add(&ready_queue, writer);
  } else {
    // Wake up all waiting readers.
    // Count and claim reader slots while holding the lock so writers cannot slip in.
    unsigned wake_count = rwlock->waiting_readers.size;
    struct TCB* readers = queue_remove_all(&rwlock->waiting_readers);
    rwlock->readers += wake_count;
    spin_lock_release(&rwlock->lock);

    while (readers != NULL){
      struct TCB* next = readers->next;
      readers->next = NULL;
      spin_queue_add(&ready_queue, readers);
      readers = next;
    }
  }
}
