#ifndef BLOCKING_RINGBUF_H
#define BLOCKING_RINGBUF_H

#include "atomic.h"
#include "semaphore.h"

struct BlockingRingBuf {
  char* buf;
  struct SpinLock spinlock;
  struct Semaphore add_sem;
  struct Semaphore remove_sem;
  unsigned capacity;
  unsigned head;
  unsigned tail;
  int size;
};

// Allocate backing storage and initialize an empty byte FIFO.
void blocking_ringbuf_init(struct BlockingRingBuf* b, unsigned capacity);

// Append one byte, blocking while the ring is full.
void blocking_ringbuf_add(struct BlockingRingBuf* b, char byte);

// Remove and return one byte, blocking while the ring is empty.
char blocking_ringbuf_remove(struct BlockingRingBuf* b);

// Return the current number of queued bytes.
unsigned blocking_ringbuf_size(struct BlockingRingBuf* b);

// Destroy owned storage and reap blocked waiters.
void blocking_ringbuf_destroy(struct BlockingRingBuf* b);

// Destroy the ring and free the struct itself.
void blocking_ringbuf_free(struct BlockingRingBuf* b);

#endif // BLOCKING_RINGBUF_H
