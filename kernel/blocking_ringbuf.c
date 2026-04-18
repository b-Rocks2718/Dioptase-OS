#include "blocking_ringbuf.h"

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

void blocking_ringbuf_init(struct BlockingRingBuf* b, unsigned capacity){
  assert(b != NULL, "blocking_ringbuf_init: ring pointer was NULL.\n");
  assert(capacity <= (unsigned)INT_MAX,
    "blocking_ringbuf_init: capacity must fit in semaphore counts.\n");

  b->buf = NULL;
  if (capacity > 0){
    b->buf = malloc(capacity);
    assert(b->buf != NULL,
      "blocking_ringbuf_init: failed to allocate ring storage.\n");
  }

  spin_lock_init(&b->spinlock);
  sem_init(&b->add_sem, capacity);
  sem_init(&b->remove_sem, 0);
  b->capacity = capacity;
  b->head = 0;
  b->tail = 0;
  __atomic_store_n(&b->size, 0);
}

void blocking_ringbuf_add(struct BlockingRingBuf* b, char byte){
  sem_down(&b->add_sem);

  spin_lock_acquire(&b->spinlock);
  assert((unsigned)b->size < b->capacity,
    "blocking_ringbuf_add: free-slot permit had no matching ring space.\n");

  b->buf[b->tail] = byte;
  b->tail = blocking_ringbuf_next_idx(b, b->tail);
  __atomic_fetch_add(&b->size, 1);

  spin_lock_release(&b->spinlock);
  sem_up(&b->remove_sem);
}

char blocking_ringbuf_remove(struct BlockingRingBuf* b){
  sem_down(&b->remove_sem);

  spin_lock_acquire(&b->spinlock);
  assert(b->size > 0,
    "blocking_ringbuf_remove: data permit had no matching queued byte.\n");

  char byte = b->buf[b->head];
  b->head = blocking_ringbuf_next_idx(b, b->head);
  __atomic_fetch_add(&b->size, -1);

  spin_lock_release(&b->spinlock);
  sem_up(&b->add_sem);
  return byte;
}

unsigned blocking_ringbuf_size(struct BlockingRingBuf* b){
  return (unsigned)__atomic_load_n(&b->size);
}

void blocking_ringbuf_destroy(struct BlockingRingBuf* b){
  assert(b != NULL, "blocking_ringbuf_destroy: ring pointer was NULL.\n");

  sem_destroy(&b->add_sem);
  sem_destroy(&b->remove_sem);
  free(b->buf);
  b->buf = NULL;
  b->capacity = 0;
  b->head = 0;
  b->tail = 0;
  __atomic_store_n(&b->size, 0);
}

void blocking_ringbuf_free(struct BlockingRingBuf* b){
  blocking_ringbuf_destroy(b);
  free(b);
}
