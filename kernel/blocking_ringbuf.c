#include "blocking_ringbuf.h"

#include "atomic.h"
#include "debug.h"
#include "heap.h"

// Advance one ring index, wrapping at capacity.
static unsigned blocking_ringbuf_next_idx(struct BlockingRingBuf* b,
    unsigned idx){
  assert(b->capacity > 0,
    "blocking_ringbuf: internal index advance on zero-capacity ring.\n");

  idx++;
  if (idx == b->capacity){
    return 0;
  }
  return idx;
}

// Initialize a byte ring and the semaphores used by its two directions.
void blocking_ringbuf_init(struct BlockingRingBuf* b, unsigned capacity){
  assert(b != NULL, "blocking_ringbuf_init: ring pointer was NULL.\n");

  b->buf = NULL;
  if (capacity > 0){
    b->buf = malloc(capacity);
    assert(b->buf != NULL,
      "blocking_ringbuf_init: failed to allocate ring storage.\n");
  }

  blocking_lock_init(&b->lock);
  cond_var_init(&b->space_available);
  cond_var_init(&b->data_available);
  b->capacity = capacity;
  b->head = 0;
  b->tail = 0;
  __atomic_store_n(&b->size, 0);
  b->producers_open = true;
  b->consumers_open = true;
}

// Enqueue one byte, or report that producers or consumers have closed.
bool blocking_ringbuf_add_fallible(struct BlockingRingBuf* b, char byte){
  assert(b != NULL, "blocking_ringbuf add: ring pointer was NULL.\n");

  /*
   * This function executes in kernel mode and may block. The BlockingLock
   * disables preemption only while this thread owns the predicate state;
   * cond_var_wait() releases it before blocking and reacquires it before
   * returning. Under the documented sequentially-consistent memory model,
   * close publication and byte publication therefore have one total order.
   *
   * Postcondition on true: exactly one byte was appended before the matching
   * data wakeup. Postcondition on false: no byte was appended by this call.
   */
  blocking_lock_acquire(&b->lock);
  while ((unsigned)__atomic_load_n(&b->size) == b->capacity &&
      b->producers_open && b->consumers_open){
    cond_var_wait(&b->space_available, &b->lock);
  }

  if (!b->producers_open || !b->consumers_open){
    blocking_lock_release(&b->lock);
    return false;
  }

  assert((unsigned)b->size < b->capacity,
    "blocking_ringbuf add: open ring had no free byte after its wait predicate completed.\n");

  b->buf[b->tail] = byte;
  b->tail = blocking_ringbuf_next_idx(b, b->tail);
  __atomic_fetch_add(&b->size, 1);

  // Publish the predicate before waking one reader. The caller still holds the
  // external lock as required by CondVar's signal contract.
  cond_var_signal(&b->data_available, &b->lock);
  blocking_lock_release(&b->lock);
  return true;
}

// Dequeue one byte, or report that producers or consumers have closed.
bool blocking_ringbuf_remove_fallible(struct BlockingRingBuf* b, char* byte){
  assert(b != NULL, "blocking_ringbuf remove: ring pointer was NULL.\n");
  assert(byte != NULL, "blocking_ringbuf remove: destination pointer was NULL.\n");

  /*
   * A producer close wakes empty readers. A reader that observes queued data
   * first consumes it even after producers close; EOF is reported only after
   * the queue reaches zero. Consumer close instead cancels immediately and
   * leaves any queued bytes untouched because no logical reader remains.
   *
   * Postcondition on true: exactly one queued byte was copied to `*byte` and
   * one writer was notified of the new slot. On false, the ring is unchanged.
   */
  blocking_lock_acquire(&b->lock);
  while (__atomic_load_n(&b->size) == 0 && b->producers_open &&
      b->consumers_open){
    cond_var_wait(&b->data_available, &b->lock);
  }

  if (!b->consumers_open || __atomic_load_n(&b->size) == 0){
    blocking_lock_release(&b->lock);
    return false;
  }

  assert(b->size > 0,
    "blocking_ringbuf remove: open ring had no queued byte after its wait predicate completed.\n");

  *byte = b->buf[b->head];
  b->head = blocking_ringbuf_next_idx(b, b->head);
  __atomic_fetch_add(&b->size, -1);

  cond_var_signal(&b->space_available, &b->lock);
  blocking_lock_release(&b->lock);
  return true;
}

// Enqueue one byte, blocking until capacity or closure is observed.
void blocking_ringbuf_add(struct BlockingRingBuf* b, char byte){
  assert(blocking_ringbuf_add_fallible(b, byte),
    "blocking_ringbuf_add: a generic producer or consumer was closed during an invariant-enforcing add.\n");
}

// Dequeue one byte, blocking until data or closure is observed.
char blocking_ringbuf_remove(struct BlockingRingBuf* b){
  char byte = 0;
  assert(blocking_ringbuf_remove_fallible(b, &byte),
    "blocking_ringbuf_remove: a generic producer or consumer was closed during an invariant-enforcing remove.\n");
  return byte;
}

// Close the producer side and wake consumers waiting for more data.
void blocking_ringbuf_close_producers(struct BlockingRingBuf* b){
  assert(b != NULL,
    "blocking_ringbuf close producers: ring pointer was NULL.\n");

  blocking_lock_acquire(&b->lock);
  b->producers_open = false;

  // Empty readers can now return EOF. Wake every waiter because each one must
  // re-check the shared size/closure predicate while holding this same lock.
  cond_var_broadcast(&b->data_available, &b->lock);
  cond_var_broadcast(&b->space_available, &b->lock);
  blocking_lock_release(&b->lock);
}

// Close the consumer side and wake producers waiting for capacity.
void blocking_ringbuf_close_consumers(struct BlockingRingBuf* b){
  assert(b != NULL,
    "blocking_ringbuf close consumers: ring pointer was NULL.\n");

  blocking_lock_acquire(&b->lock);
  b->consumers_open = false;

  // Full writers can now return their committed byte count instead of
  // remaining blocked forever. Also wake any invalid same-side operation so
  // the fallible API consistently observes closure.
  cond_var_broadcast(&b->space_available, &b->lock);
  cond_var_broadcast(&b->data_available, &b->lock);
  blocking_lock_release(&b->lock);
}

// Return the number of bytes currently buffered.
unsigned blocking_ringbuf_size(struct BlockingRingBuf* b){
  return (unsigned)__atomic_load_n(&b->size);
}

// Destroy the ring and synchronization state after both sides quiesce.
void blocking_ringbuf_destroy(struct BlockingRingBuf* b){
  assert(b != NULL, "blocking_ringbuf_destroy: ring pointer was NULL.\n");

  /*
   * Preconditions: the owner has prevented new operations; every blocked
   * fallible operation has been released through a close call and returned;
   * and no close/size/add/remove call is active. CondVar and BlockingLock
   * destruction enforce their component portions of this invariant.
   */
  cond_var_destroy(&b->space_available);
  cond_var_destroy(&b->data_available);
  blocking_lock_destroy(&b->lock);
  free(b->buf);
  b->buf = NULL;
  b->capacity = 0;
  b->head = 0;
  b->tail = 0;
  __atomic_store_n(&b->size, 0);
  b->producers_open = false;
  b->consumers_open = false;
}

// Destroy and free a heap-allocated blocking ring.
void blocking_ringbuf_free(struct BlockingRingBuf* b){
  blocking_ringbuf_destroy(b);
  free(b);
}
