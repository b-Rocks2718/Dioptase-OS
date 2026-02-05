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

void blocking_queue_init(struct BlockingQueue* b);

void blocking_queue_add(struct BlockingQueue* b, struct GenericQueueElement* element);

struct GenericQueueElement* blocking_queue_remove(struct BlockingQueue* b);

struct GenericQueueElement* blocking_queue_remove_all(struct BlockingQueue* b);

unsigned blocking_queue_size(struct BlockingQueue* b);

#endif // BLOCKING_QUEUE_H