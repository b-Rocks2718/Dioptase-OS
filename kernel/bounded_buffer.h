#ifndef BOUNDED_BUFFER_H
#define BOUNDED_BUFFER_H

#include "queue.h"
#include "semaphore.h"

// blocking queue with a fixed maximum capacity
struct BoundedBuffer {
  struct GenericSpinQueue queue;
  struct Semaphore add_sem; // available slots
  struct Semaphore remove_sem; // available items
  unsigned capacity;
};

// initialize an empty bounded buffer with the given capacity
void bounded_buffer_init(struct BoundedBuffer* b, unsigned capacity);

// append an element, blocking while the buffer is full
void bounded_buffer_add(struct BoundedBuffer* b, struct GenericQueueElement* element);

// remove and return one element, blocking while the buffer is empty
struct GenericQueueElement* bounded_buffer_remove(struct BoundedBuffer* b);

// detach and return the current contents without blocking
struct GenericQueueElement* bounded_buffer_remove_all(struct BoundedBuffer* b);

// return the current number of queued elements
unsigned bounded_buffer_size(struct BoundedBuffer* b);

#endif // BOUNDED_BUFFER_H
