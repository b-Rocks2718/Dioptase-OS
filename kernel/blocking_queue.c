#include "blocking_queue.h"
#include "debug.h"

// port of Gheith kernel implementation

void blocking_queue_init(struct BlockingQueue* b) {
  generic_spin_queue_init(&b->queue);
  sem_init(&b->sem, 0);
}

void blocking_queue_add(struct BlockingQueue* b, struct GenericQueueElement* element) {
  assert(element != NULL, "Cannot add NULL element to blocking queue.\n");
  generic_spin_queue_add(&b->queue, element);
  sem_up(&b->sem);
}

struct GenericQueueElement* blocking_queue_remove(struct BlockingQueue* b) {
  struct GenericQueueElement* element = NULL;
  while (element == NULL) {
    // Block until an element is available
    sem_down(&b->sem);
    // Try to remove an element from the queue
    element = generic_spin_queue_remove(&b->queue);
  }
  return element;
}

struct GenericQueueElement* blocking_queue_remove_all(struct BlockingQueue* b){
  return generic_spin_queue_remove_all(&b->queue);
}

unsigned blocking_queue_size(struct BlockingQueue* b) {
  return generic_spin_queue_size(&b->queue);
}
