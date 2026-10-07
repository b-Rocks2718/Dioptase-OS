#include "bounded_buffer.h"
#include "debug.h"
#include "constants.h"
#include "machine.h"
#include "heap.h"
#include "print.h"

// initialize queue state plus the slot/item semaphores
void bounded_buffer_init(struct BoundedBuffer* b, unsigned capacity) {
  generic_spin_queue_init(&b->queue);
  sem_init(&b->add_sem, capacity);
  sem_init(&b->remove_sem, 0);
  b->capacity = capacity;
}

// Destroy the buffer's queue and semaphores after its users have stopped.
void bounded_buffer_destroy(struct BoundedBuffer* b) {
  assert(b != NULL, "bounded_buffer_destroy: buffer is NULL.\n");

  // Check caller-owned payloads before tearing down either semaphore so a
  // lifecycle violation leaves the composite state intact for diagnostics.
  generic_spin_queue_assert_quiescent(&b->queue);
  sem_assert_destroyable(&b->add_sem);
  sem_assert_destroyable(&b->remove_sem);
  unsigned size = bounded_buffer_size(b);
  int add_permits = __atomic_load_n(&b->add_sem.count);
  int remove_permits = __atomic_load_n(&b->remove_sem.count);
  if (size != 0 || remove_permits != 0 ||
      add_permits != (int)b->capacity || b->queue.head != NULL ||
      b->queue.tail != NULL){
    int args[7] = {(int)b, (int)size, (int)b->capacity, add_permits,
      remove_permits, (int)b->queue.head, (int)b->queue.tail};
    say("| bounded_buffer: destroy rejected buffer=0x%X size=%d capacity=%d add_permits=%d remove_permits=%d head=0x%X tail=0x%X\n",
      args);
    panic("bounded_buffer_destroy: owner must drain every payload and restore slot permits before destruction.\n");
  }

  sem_destroy(&b->add_sem);
  sem_destroy(&b->remove_sem);
  generic_spin_queue_destroy(&b->queue);
  b->capacity = 0;
}

// Destroy and free a heap-allocated bounded buffer.
void bounded_buffer_free(struct BoundedBuffer* b) {
  bounded_buffer_destroy(b);
  free(b);
}

// Block for a free slot, then enqueue one element.
//
// add_sem counts free slots and remove_sem counts published elements. Every
// element is queued before its remove permit is published and dequeued before
// its slot permit is returned, so queue size + add permits <= capacity and
// queue size >= remove permits at all times. Holding a permit therefore
// guarantees the matching queue operation succeeds.
void bounded_buffer_add(struct BoundedBuffer* b, struct GenericQueueElement* element) {
  assert(element != NULL, "bounded_buffer_add: cannot add a NULL element.\n");
  sem_down(&b->add_sem);
  generic_spin_queue_add(&b->queue, element);
  sem_up(&b->remove_sem);
}

// Block for a published element, then dequeue it and return its slot.
struct GenericQueueElement* bounded_buffer_remove(struct BoundedBuffer* b) {
  sem_down(&b->remove_sem);
  struct GenericQueueElement* element = generic_spin_queue_remove(&b->queue);
  assert_always(element != NULL,
    "bounded_buffer_remove: semaphore permit had no matching queued element.\n");
  sem_up(&b->add_sem);
  return element;
}

// Detach all currently queued elements and restore available capacity.
struct GenericQueueElement* bounded_buffer_remove_all(struct BoundedBuffer* b){
  struct GenericQueueElement* head = NULL;
  struct GenericQueueElement* tail = NULL;

  // Drain only the items that already have published remove permits. This keeps
  // remove_sem aligned with the queue contents and returns one slot permit per
  // removed element so the buffer can be reused immediately afterward.
  while (sem_try_down(&b->remove_sem)) {
    struct GenericQueueElement* element = generic_spin_queue_remove(&b->queue);
    assert(element != NULL,
      "bounded_buffer_remove_all: semaphore permit had no matching queued element.\n");

    if (head == NULL) {
      head = element;
      tail = element;
    } else {
      tail->next = element;
      tail = element;
    }

    sem_up(&b->add_sem);
  }

  return head;
}

// Return the number of elements currently buffered.
unsigned bounded_buffer_size(struct BoundedBuffer* b) {
  return generic_spin_queue_size(&b->queue);
}
