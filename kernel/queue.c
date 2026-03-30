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
  // Queue insertion always consumes a single detached node. Semaphore waiters
  // may have stale linkage from an earlier queue, so clear it before linking.
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
  queue->size = 0;
}

// block() uses this callback shape, so args packs { queue, tcb }.
// Insert in wakeup order and keep equal deadlines in FIFO order.
void sleep_queue_add(void* args){
  int* args_array = (int*)args;
  struct SleepQueue* queue = (struct SleepQueue*)args_array[0];
  struct TCB* data = (struct TCB*)args_array[1];

  // Sleep queue insertion also consumes a detached node. Clear any stale
  // linkage before we splice the thread into the ordered wakeup list.
  data->next = NULL;

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
}

// The list is sorted, so only the head can be ready to wake.
struct TCB* sleep_queue_remove(struct SleepQueue* queue){

  if (queue->head == NULL){
    return NULL;
  }

  struct TCB* node = queue->head;
  if (node->wakeup_jiffies <= current_jiffies) {
    // remove from sleep queue
    queue->head = node->next;
    node->next = NULL;

    __atomic_fetch_add(&queue->size, -1);

    return node;
  } else {
    // not ready to wake up
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


// The ring buffer leaves one slot empty so head == tail means empty.
void ringbuf_init(struct RingBuf* rb, unsigned capacity){
  rb->buf = malloc(capacity * sizeof(void*));
  rb->capacity = capacity;
  rb->head = 0;
  rb->tail = 0;
}

// Front pushes advance head.
bool ringbuf_add_front(struct RingBuf* rb, void* p){
  if ((rb->head + 1) % rb->capacity == rb->tail){
    // full
    return false;
  }

  rb->buf[rb->head] = p;
  rb->head = (rb->head + 1) % rb->capacity;
  return true;
}

// Back pushes retreat tail.
bool ringbuf_add_back(struct RingBuf* rb, void* p){
  if ((rb->head + 1) % rb->capacity == rb->tail){
    // full
    return false;
  }

  rb->tail = (rb->tail - 1 + rb->capacity) % rb->capacity;
  rb->buf[rb->tail] = p;
  return true;
}

// Front pops retreat head.
void* ringbuf_remove_front(struct RingBuf* rb){
  if (rb->head == rb->tail){
    // empty
    return NULL;
  }

  rb->head = (rb->head - 1 + rb->capacity) % rb->capacity;
  return rb->buf[rb->head];
}

// Back pops advance tail.
void* ringbuf_remove_back(struct RingBuf* rb){
  if (rb->head == rb->tail){
    // empty
    return NULL;
  }

  void* c = rb->buf[rb->tail];
  rb->tail = (rb->tail + 1) % rb->capacity;
  return c;
}

unsigned ringbuf_size(struct RingBuf* rb){
  if (rb->head >= rb->tail){
    return rb->head - rb->tail;
  } else {
    return rb->capacity - (rb->tail - rb->head);
  }
}

void ringbuf_destroy(struct RingBuf* rb){
  // free dynamically allocated buffer
  free(rb->buf);
  rb->buf = NULL;
  rb->capacity = 0;
  rb->head = 0;
  rb->tail = 0;
}

void ringbuf_free(struct RingBuf* rb){
  ringbuf_destroy(rb);
  free(rb);
}


// initialize keybuf
void keybuf_init(struct KeyBuf* kb){
  for (int i = 0; i < KEYBUF_CAPACITY; i++){
    kb->buf[i] = 0;
  }
  kb->head = 0;
  kb->tail = 0;
}

// push an element at the front; returns false if the buffer is full
bool keybuf_add(struct KeyBuf* kb, short p){
  if ((kb->head + 1) % KEYBUF_CAPACITY == kb->tail){
    // full
    return false;
  }

  kb->buf[kb->head] = p;
  kb->head = (kb->head + 1) % KEYBUF_CAPACITY;
  return true;
}

// pop and return the back element, or 0 if empty
short keybuf_remove(struct KeyBuf* kb){
  if (kb->head == kb->tail){
    // empty
    return 0;
  }

  // Consume the current tail slot before advancing it. This matches the
  // one-empty-slot FIFO invariant used by keybuf_add() and preserves the
  // oldest queued key for the single consumer.
  short key = kb->buf[kb->tail];
  kb->tail = (kb->tail + 1) % KEYBUF_CAPACITY;
  return key;
}

// return the current number of stored elements
unsigned keybuf_size(struct KeyBuf* kb){
  if (kb->head >= kb->tail){
    return kb->head - kb->tail;
  } else {
    return KEYBUF_CAPACITY - (kb->tail - kb->head);
  }
}
