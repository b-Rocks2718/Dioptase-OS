#include "queue.h"
#include "heap.h"
#include "atomic.h"
#include "machine.h"
#include "constants.h"
#include "pit.h"
#include "debug.h"

void spin_queue_init(struct SpinQueue* queue){
  queue->head = NULL;
  queue->tail = NULL;
  spin_lock_init(&queue->spinlock);
  queue->size = 0;
}

void spin_queue_add(struct SpinQueue* queue, struct TCB* data){
  assert(data != NULL, "spin_queue_add: data is NULL.\n");
  spin_lock_acquire(&queue->spinlock);

  // Queue insertion always consumes a single detached node.
  // Force next=NULL to avoid linking stale list tails into this queue.
  data->next = NULL;

  if (queue->head == NULL){
    queue->head = data;
    queue->tail = data;
  } else {
    queue->tail->next = data;
    queue->tail = data;
  }

  __atomic_fetch_add(&queue->size, 1);

  spin_lock_release(&queue->spinlock);
}

struct TCB* spin_queue_remove(struct SpinQueue* queue){
  spin_lock_acquire(&queue->spinlock);

  if (queue->head == NULL){
    spin_lock_release(&queue->spinlock);
    return NULL;
  }

  struct TCB* node = queue->head;
  queue->head = node->next;
  node->next = NULL;

  if (queue->head == NULL){
    queue->tail = NULL;
  }

  __atomic_fetch_add(&queue->size, -1);

  spin_lock_release(&queue->spinlock);

  return node;
}

struct TCB* spin_queue_remove_all(struct SpinQueue* queue){
  spin_lock_acquire(&queue->spinlock);

  struct TCB* head = queue->head;
  queue->head = NULL;
  queue->tail = NULL;
  __atomic_store_n(&queue->size, 0);

  spin_lock_release(&queue->spinlock);

  return head;
}

unsigned spin_queue_size(struct SpinQueue* queue){
  return __atomic_load_n(&queue->size);
}

struct TCB* spin_queue_peek(struct SpinQueue* queue){
  spin_lock_acquire(&queue->spinlock);

  struct TCB* head = queue->head;

  spin_lock_release(&queue->spinlock);

  return head;
}


void queue_init(struct Queue* queue){
  queue->head = NULL;
  queue->tail = NULL;
  queue->size = 0;
}

void queue_add(struct Queue* queue, struct TCB* data){
  if (queue->head == NULL){
    queue->head = data;
    queue->tail = data;
  } else {
    queue->tail->next = data;
    queue->tail = data;
  }

  __atomic_fetch_add(&queue->size, 1);
}

struct TCB* queue_remove(struct Queue* queue){
  if (queue->head == NULL){
    return NULL;
  }

  struct TCB* node = queue->head;
  queue->head = node->next;
  node->next = NULL;

  if (queue->head == NULL){
    queue->tail = NULL;
  }

  __atomic_fetch_add(&queue->size, -1);

  return node;
}

struct TCB* queue_remove_all(struct Queue* queue){
  struct TCB* head = queue->head;
  queue->head = NULL;
  queue->tail = NULL;
  __atomic_store_n(&queue->size, 0);

  return head;
}

unsigned queue_size(struct Queue* queue){
  return __atomic_load_n(&queue->size);
}

struct TCB* queue_peek(struct Queue* queue){
  return queue->head;
}


void sleep_queue_init(struct SleepQueue* queue){
  queue->head = NULL;
  spin_lock_init(&queue->spinlock);
  queue->size = 0;
}

// Adds a TCB to the sleep queue.
// sort by wakeup_jiffies
void sleep_queue_add(void* args){
  int* args_array = (int*)args;
  struct SleepQueue* queue = (struct SleepQueue*)args_array[0];
  struct TCB* data = (struct TCB*)args_array[1];

  spin_lock_acquire(&queue->spinlock);

  if (queue->head == NULL){
    // empty queue
    queue->head = data;
  } else {
    // non-empty queue
    struct TCB* current = queue->head;
    struct TCB* previous = NULL;

    while (current != NULL && current->wakeup_jiffies <= data->wakeup_jiffies) {
      previous = current;
      current = current->next;
    }

    if (previous == NULL) {
      // insert at head
      data->next = queue->head;
      queue->head = data;
    } else {
      // insert in middle or end
      previous->next = data;
      data->next = current;
    }
  }

  __atomic_fetch_add(&queue->size, 1);

  spin_lock_release(&queue->spinlock);
}

// return first TCB if its wakeup_jiffies <= current jiffies
// else return NULL
struct TCB* sleep_queue_remove(struct SleepQueue* queue){
  spin_lock_acquire(&queue->spinlock);

  if (queue->head == NULL){
    spin_lock_release(&queue->spinlock);
    return NULL;
  }

  struct TCB* node = queue->head;
  if (node->wakeup_jiffies <= current_jiffies) {
    // remove from sleep queue
    queue->head = node->next;
    node->next = NULL;

    __atomic_fetch_add(&queue->size, -1);

    spin_lock_release(&queue->spinlock);

    return node;
  } else {
    // not ready to wake up
    spin_lock_release(&queue->spinlock);
    return NULL;
  }
}

unsigned sleep_queue_size(struct SleepQueue* queue){
  return __atomic_load_n(&queue->size);
}


void generic_queue_init(struct GenericQueue* queue){
  queue->head = NULL;
  queue->tail = NULL;
  queue->size = 0;
}

void generic_queue_add(struct GenericQueue* queue, struct GenericQueueElement* data){
  assert(data != NULL, "generic_queue_add: data is NULL.\n");

  // Queue insertion always consumes a single detached node.
  // Force next=NULL to avoid linking stale list tails into this queue.
  data->next = NULL;

  if (queue->head == NULL){
    queue->head = data;
    queue->tail = data;
  } else {
    queue->tail->next = data;
    queue->tail = data;
  }

  __atomic_fetch_add(&queue->size, 1);
}

struct GenericQueueElement* generic_queue_remove(struct GenericQueue* queue){
  if (queue->head == NULL){
    return NULL;
  }

  struct GenericQueueElement* node = queue->head;
  queue->head = node->next;
  node->next = NULL;

  if (queue->head == NULL){
    queue->tail = NULL;
  }

  __atomic_fetch_add(&queue->size, -1);

  return node;
}

struct GenericQueueElement* generic_queue_remove_all(struct GenericQueue* queue){
  struct GenericQueueElement* head = queue->head;
  queue->head = NULL;
  queue->tail = NULL;
  __atomic_store_n(&queue->size, 0);

  return head;
}

unsigned generic_queue_size(struct GenericQueue* queue){
  return __atomic_load_n(&queue->size);
}


void generic_spin_queue_init(struct GenericSpinQueue* queue){
  queue->head = NULL;
  queue->tail = NULL;
  spin_lock_init(&queue->spinlock);
  queue->size = 0;
}

void generic_spin_queue_add(struct GenericSpinQueue* queue, struct GenericQueueElement* data){
  assert(data != NULL, "generic_spin_queue_add: data is NULL.\n");
  spin_lock_acquire(&queue->spinlock);

  // Queue insertion always consumes a single detached node.
  // Force next=NULL to avoid linking stale list tails into this queue.
  data->next = NULL;

  if (queue->head == NULL){
    queue->head = data;
    queue->tail = data;
  } else {
    queue->tail->next = data;
    queue->tail = data;
  }

  __atomic_fetch_add(&queue->size, 1);

  spin_lock_release(&queue->spinlock);
}

struct GenericQueueElement* generic_spin_queue_remove(struct GenericSpinQueue* queue){
  spin_lock_acquire(&queue->spinlock);

  if (queue->head == NULL){
    spin_lock_release(&queue->spinlock);
    return NULL;
  }

  struct GenericQueueElement* node = queue->head;
  queue->head = node->next;
  node->next = NULL;

  if (queue->head == NULL){
    queue->tail = NULL;
  }

  __atomic_fetch_add(&queue->size, -1);

  spin_lock_release(&queue->spinlock);

  return node;
}

struct GenericQueueElement* generic_spin_queue_remove_all(struct GenericSpinQueue* queue){
  spin_lock_acquire(&queue->spinlock);

  struct GenericQueueElement* head = queue->head;
  queue->head = NULL;
  queue->tail = NULL;
  __atomic_store_n(&queue->size, 0);

  spin_lock_release(&queue->spinlock);

  return head;
}

unsigned generic_spin_queue_size(struct GenericSpinQueue* queue){
  return __atomic_load_n(&queue->size);
}
