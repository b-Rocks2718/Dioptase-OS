#ifndef BOUNDED_BUFFER_H
#define BOUNDED_BUFFER_H

#include "queue.h"
#include "semaphore.h"

// Same as BlockingQueue but with a maximum capacity
struct BoundedBuffer {
  struct GenericSpinQueue queue;
  struct Semaphore add_sem;
  struct Semaphore remove_sem;
  unsigned capacity;
};

void bounded_buffer_init(struct BoundedBuffer* b, unsigned capacity);

void bounded_buffer_add(struct BoundedBuffer* b, struct GenericQueueElement* element);

struct GenericQueueElement* bounded_buffer_remove(struct BoundedBuffer* b);

struct GenericQueueElement* bounded_buffer_remove_all(struct BoundedBuffer* b);

unsigned bounded_buffer_size(struct BoundedBuffer* b);

#endif // BOUNDED_BUFFER_H