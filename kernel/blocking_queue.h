#ifndef BLOCKING_QUEUE_H
#define BLOCKING_QUEUE_H

#include "queue.h"
#include "semaphore.h"

// port of Gheith kernel implementation

// Queue that blocks on remove when empty
struct BlockingQueue {
  struct GenericSpinQueue queue;
  struct Semaphore sem;
};

// initialize an empty blocking queue
void blocking_queue_init(struct BlockingQueue* b);

// append an element and wake one blocked remover
void blocking_queue_add(struct BlockingQueue* b, struct GenericQueueElement* element);

// remove and return one element, blocking while the queue is empty
struct GenericQueueElement* blocking_queue_remove(struct BlockingQueue* b);

// try to remove and return one currently available element, returning NULL if
// no unreserved item is available without blocking
struct GenericQueueElement* blocking_queue_try_remove(struct BlockingQueue* b);

// detach and return all currently available elements without blocking
struct GenericQueueElement* blocking_queue_remove_all(struct BlockingQueue* b);

// return the current number of queued elements
unsigned blocking_queue_size(struct BlockingQueue* b);

#endif // BLOCKING_QUEUE_H
