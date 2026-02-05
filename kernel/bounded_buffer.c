#include "bounded_buffer.h"
#include "debug.h"
#include "constants.h"
#include "machine.h"

void bounded_buffer_init(struct BoundedBuffer* b, unsigned capacity) {
  generic_spin_queue_init(&b->queue);
  sem_init(&b->add_sem, capacity);
  sem_init(&b->remove_sem, 0);
  b->capacity = capacity;
}

void bounded_buffer_add(struct BoundedBuffer* b, struct GenericQueueElement* element) {
  assert(element != NULL, "Cannot add NULL element to blocking queue.\n");
  bool added = false;
  while (!added){
    sem_down(&b->add_sem);

    spin_lock_acquire(&b->queue.spinlock);
    if (b->queue.size < b->capacity) {
      // add to queue

      // not using generic_spin_queue_add to avoid double locking
      if (b->queue.tail == NULL) {
        b->queue.head = element;
        b->queue.tail = element;
      } else {
        b->queue.tail->next = element;
        b->queue.tail = element;
      }
      element->next = NULL;
      __atomic_fetch_add(&b->queue.size, 1);

      added = true;
    }
    spin_lock_release(&b->queue.spinlock);
  }

  sem_up(&b->remove_sem);
}

struct GenericQueueElement* bounded_buffer_remove(struct BoundedBuffer* b) {
  struct GenericQueueElement* element = NULL;
  while (element == NULL) {
    // Block until an element is available
    sem_down(&b->remove_sem);
    // Try to remove an element from the queue
    element = generic_spin_queue_remove(&b->queue);
  }
  sem_up(&b->add_sem);
  return element;
}

struct GenericQueueElement* bounded_buffer_remove_all(struct BoundedBuffer* b){
  return generic_spin_queue_remove_all(&b->queue);
}

unsigned bounded_buffer_size(struct BoundedBuffer* b) {
  return generic_spin_queue_size(&b->queue);
}
