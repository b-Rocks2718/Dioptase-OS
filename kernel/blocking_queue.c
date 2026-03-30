#include "blocking_queue.h"
#include "debug.h"

// port of Gheith kernel implementation

// initialize queue state and zero the available-item count
void blocking_queue_init(struct BlockingQueue* b) {
  generic_spin_queue_init(&b->queue);
  sem_init(&b->sem, 0);
}

// enqueue one element and publish one available item
void blocking_queue_add(struct BlockingQueue* b, struct GenericQueueElement* element) {
  assert(element != NULL, "Cannot add NULL element to blocking queue.\n");
  generic_spin_queue_add(&b->queue, element);
  sem_up(&b->sem);
}

// Once sem_down() returns, one queued element must be reserved for this caller.
struct GenericQueueElement* blocking_queue_remove(struct BlockingQueue* b) {
  sem_down(&b->sem);
  struct GenericQueueElement* element = generic_spin_queue_remove(&b->queue);
  assert(element != NULL,
    "blocking_queue_remove: semaphore permit had no matching queued element.\n");
  return element;
}

struct GenericQueueElement* blocking_queue_try_remove(struct BlockingQueue* b) {
  if (!sem_try_down(&b->sem)) {
    return NULL;
  }

  struct GenericQueueElement* element = generic_spin_queue_remove(&b->queue);
  assert(element != NULL,
    "blocking_queue_try_remove: semaphore permit had no matching queued element.\n");
  return element;
}

struct GenericQueueElement* blocking_queue_remove_all(struct BlockingQueue* b){
  struct GenericQueueElement* head = NULL;
  struct GenericQueueElement* tail = NULL;

  struct GenericQueueElement* element = blocking_queue_try_remove(b);
  while (element != NULL) {
    if (head == NULL) {
      head = element;
      tail = element;
    } else {
      tail->next = element;
      tail = element;
    }

    element = blocking_queue_try_remove(b);
  }

  return head;
}

unsigned blocking_queue_size(struct BlockingQueue* b) {
  return generic_spin_queue_size(&b->queue);
}
