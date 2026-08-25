#include "blocking_queue.h"
#include "debug.h"
#include "print.h"

// initialize queue state and zero the available-item count
void blocking_queue_init(struct BlockingQueue* b) {
  generic_spin_queue_init(&b->queue);
  sem_init(&b->sem, 0);
}

void blocking_queue_destroy(struct BlockingQueue* b) {
  assert(b != NULL, "blocking_queue_destroy: queue is NULL.\n");

  // Check payload ownership before destroying the semaphore. This keeps a
  // rejected teardown from partially dismantling the composite object.
  generic_spin_queue_assert_quiescent(&b->queue);
  sem_assert_destroyable(&b->sem);
  unsigned size = blocking_queue_size(b);
  int permits = __atomic_load_n(&b->sem.count);
  if (size != 0 || permits != 0 || b->queue.head != NULL ||
      b->queue.tail != NULL){
    int args[5] = {(int)b, (int)size, permits, (int)b->queue.head,
      (int)b->queue.tail};
    say("| blocking_queue: destroy rejected queue=0x%X size=%d permits=%d head=0x%X tail=0x%X\n",
      args);
    panic("blocking_queue_destroy: owner must drain every payload and permit before destruction.\n");
  }

  sem_destroy(&b->sem);
  generic_spin_queue_destroy(&b->queue);
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
