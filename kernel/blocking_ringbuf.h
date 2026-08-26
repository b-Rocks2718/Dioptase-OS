#ifndef BLOCKING_RINGBUF_H
#define BLOCKING_RINGBUF_H

#include "blocking_lock.h"
#include "cond_var.h"

// Close-aware byte FIFO.
//
// `lock` protects the indices and the producer/consumer-open predicates.
// `size` is mutated while holding `lock`, but remains atomic so the existing
// nonblocking size snapshot can safely run without acquiring a sleeping lock.
struct BlockingRingBuf {
  char* buf;
  struct BlockingLock lock;
  struct CondVar space_available;
  struct CondVar data_available;
  unsigned capacity;
  unsigned head;
  unsigned tail;
  int size;
  bool producers_open;
  bool consumers_open;
};

// Allocate backing storage and initialize an empty byte FIFO.
void blocking_ringbuf_init(struct BlockingRingBuf* b, unsigned capacity);

// Append one byte, blocking while the ring is full.
void blocking_ringbuf_add(struct BlockingRingBuf* b, char byte);

// Remove and return one byte, blocking while the ring is empty.
char blocking_ringbuf_remove(struct BlockingRingBuf* b);

// Append one byte, blocking while the ring is full. Return false if either
// side closes before the byte is committed. This is the fallible form used by
// pipes; generic users normally use the invariant-enforcing wrapper above.
bool blocking_ringbuf_add_fallible(struct BlockingRingBuf* b, char byte);

// Remove one byte, blocking while the ring is empty and producers remain.
// Return false without modifying `*byte` at EOF, or if consumers close. Bytes
// already queued when producers close remain available and drain normally.
bool blocking_ringbuf_remove_fallible(struct BlockingRingBuf* b, char* byte);

// Publish that no producer/consumer can begin another operation, and wake all
// waiters whose predicate may now terminate. These calls are idempotent.
void blocking_ringbuf_close_producers(struct BlockingRingBuf* b);
void blocking_ringbuf_close_consumers(struct BlockingRingBuf* b);

// Return the current number of queued bytes.
unsigned blocking_ringbuf_size(struct BlockingRingBuf* b);

// Destroy owned storage after all producers/consumers have returned. The owner
// must arrange wakeup and joining; destruction never terminates waiters.
void blocking_ringbuf_destroy(struct BlockingRingBuf* b);

// Destroy the ring and free the struct itself.
void blocking_ringbuf_free(struct BlockingRingBuf* b);

#endif // BLOCKING_RINGBUF_H
