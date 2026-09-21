#include "queue.h"
#include "heap.h"
#include "atomic.h"
#include "machine.h"
#include "constants.h"
#include "pit.h"
#include "debug.h"
#include "print.h"

/*
 * The PIT timekeeper is a 32-bit wrapping counter. Sleep deadlines are limited
 * to INT_MAX ticks from their insertion point, so the high bit of the modular
 * subtraction gives an unambiguous ordering within that half-range. This uses
 * only unsigned arithmetic; it does not depend on converting an out-of-range
 * unsigned value to signed int.
 *
 * Preconditions: a and b are deadlines/current times that differ by less than
 * 2^31 ticks. sleep() and the user trap enforce that horizon for every live
 * production entry. Tests using explicit deadlines must preserve it as well.
 */
static bool jiffies_before(unsigned a, unsigned b){
  return a != b && ((a - b) & (INT_MAX + 1U)) != 0;
}

// Initialize an interrupt-safe FIFO of TCB nodes and its CLH lock.
void spin_queue_init(struct SpinQueue* queue){
  queue->head = NULL;
  queue->tail = NULL;
  clh_lock_init(&queue->spinlock);
  queue->size = 0;
}

// Release an externally quiescent, empty queue's CLH state.
void spin_queue_destroy(struct SpinQueue* queue){
  assert(queue != NULL, "spin_queue_destroy: queue is NULL.\n");

  // Queue nodes are TCBs owned by their scheduler/lifecycle state. Silently
  // clearing a nonempty queue would lose those owners and strand their stacks,
  // descriptors, or exit publication. External quiescence permits this
  // lock-free snapshot; no producer or consumer may still be active here.
  int size = __atomic_load_n(&queue->size);
  if (size != 0 || queue->head != NULL || queue->tail != NULL){
    int args[4] = {(int)queue, size, (int)queue->head, (int)queue->tail};
    say("| queue: spin_queue_destroy rejected queue=0x%X size=%d head=0x%X tail=0x%X\n",
      args);
    panic("spin_queue_destroy: owner must drain every TCB before destruction.\n");
  }

  clh_lock_destroy(&queue->spinlock);
  queue->head = NULL;
  queue->tail = NULL;
  __atomic_store_n(&queue->size, 0);
}

// Append one detached TCB to the FIFO while holding the CLH lock.
void spin_queue_add(struct SpinQueue* queue, struct TCB* data){
  assert(data != NULL, "spin_queue_add: data is NULL.\n");
  clh_lock_acquire(&queue->spinlock);

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

  clh_lock_release(&queue->spinlock);
}

// Remove and detach the oldest TCB, or return NULL when the queue is empty.
struct TCB* spin_queue_remove(struct SpinQueue* queue){
  clh_lock_acquire(&queue->spinlock);

  if (queue->head == NULL){
    clh_lock_release(&queue->spinlock);
    return NULL;
  }

  struct TCB* node = queue->head;
  queue->head = node->next;
  node->next = NULL;

  if (queue->head == NULL){
    queue->tail = NULL;
  }

  __atomic_fetch_add(&queue->size, -1);

  clh_lock_release(&queue->spinlock);

  return node;
}

// Detach the entire TCB FIFO and return its former head.
struct TCB* spin_queue_remove_all(struct SpinQueue* queue){
  clh_lock_acquire(&queue->spinlock);

  struct TCB* head = queue->head;
  queue->head = NULL;
  queue->tail = NULL;
  __atomic_store_n(&queue->size, 0);

  clh_lock_release(&queue->spinlock);

  return head;
}

// Return the atomically maintained number of TCBs in the FIFO.
unsigned spin_queue_size(struct SpinQueue* queue){
  return __atomic_load_n(&queue->size);
}

// Return the current head without removing it.
struct TCB* spin_queue_peek(struct SpinQueue* queue){
  clh_lock_acquire(&queue->spinlock);

  struct TCB* head = queue->head;

  clh_lock_release(&queue->spinlock);

  return head;
}


// Initialize an unlocked FIFO of TCB nodes.
void queue_init(struct Queue* queue){
  queue->head = NULL;
  queue->tail = NULL;
  queue->size = 0;
}

// Append one detached TCB to the FIFO; the caller supplies synchronization.
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

// Remove and detach the oldest TCB, or return NULL when empty.
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

// Detach every TCB in the FIFO and return the former head.
struct TCB* queue_remove_all(struct Queue* queue){
  struct TCB* head = queue->head;
  queue->head = NULL;
  queue->tail = NULL;
  __atomic_store_n(&queue->size, 0);

  return head;
}

// Return the atomically maintained number of queued TCBs.
unsigned queue_size(struct Queue* queue){
  return __atomic_load_n(&queue->size);
}

// Return the FIFO head without removing it.
struct TCB* queue_peek(struct Queue* queue){
  return queue->head;
}


// Initialize the per-owner FIFO ordered by TCB wakeup deadline.
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

    while (current != NULL &&
        !jiffies_before(data->wakeup_jiffies, current->wakeup_jiffies)) {
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
//
// CPU/synchronization assumption: SleepQueue has no internal lock. The caller
// must be the only mutator of this SleepQueue during the call. The production
// scheduler satisfies this through per-core sleep queue ownership; tests use
// this explicit-time helper without touching the live PIT timekeeper.
//
// Postconditions: returning a TCB means it was removed from the head, had
// next=NULL forced, and the queue size was decremented. Returning NULL leaves
// the queue unchanged.
struct TCB* sleep_queue_remove_at(struct SleepQueue* queue, unsigned now_jiffies){

  if (queue->head == NULL){
    return NULL;
  }

  struct TCB* node = queue->head;
  if (!jiffies_before(now_jiffies, node->wakeup_jiffies)) {
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

// Remove the first TCB whose deadline has passed at current_jiffies.
struct TCB* sleep_queue_remove(struct SleepQueue* queue){
  return sleep_queue_remove_at(queue, current_jiffies);
}

// Detach every sleeping TCB; the per-core owner must wake or reap them.
struct TCB* sleep_queue_remove_all(struct SleepQueue* queue){
  assert(queue != NULL, "sleep_queue_remove_all: queue is NULL.\n");

  // SleepQueue is intentionally single-owner and has no internal lock.
  // Shutdown calls this only after every core has disabled interrupts and
  // reached the barrier, so no PIT path or scheduler can mutate this list.
  struct TCB* head = queue->head;
  queue->head = NULL;
  __atomic_store_n(&queue->size, 0);
  return head;
}

// Return the atomically maintained number of sleeping TCBs.
unsigned sleep_queue_size(struct SleepQueue* queue){
  return __atomic_load_n(&queue->size);
}


// Initialize an unlocked FIFO of caller-owned generic elements.
void generic_queue_init(struct GenericQueue* queue){
  queue->head = NULL;
  queue->tail = NULL;
  queue->size = 0;
}

// Append one detached generic element; the caller supplies synchronization.
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

// Remove and detach the oldest generic element, or return NULL when empty.
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

// Detach every generic element and return the former head.
struct GenericQueueElement* generic_queue_remove_all(struct GenericQueue* queue){
  struct GenericQueueElement* head = queue->head;
  queue->head = NULL;
  queue->tail = NULL;
  __atomic_store_n(&queue->size, 0);

  return head;
}

// Return the atomically maintained number of generic elements.
unsigned generic_queue_size(struct GenericQueue* queue){
  return __atomic_load_n(&queue->size);
}


// Initialize a CLH-protected FIFO of generic elements.
void generic_spin_queue_init(struct GenericSpinQueue* queue){
  queue->head = NULL;
  queue->tail = NULL;
  clh_lock_init(&queue->spinlock);
  queue->size = 0;
  __atomic_store_n(&queue->active_operations, 0);
}

// Require that no operation or CLH waiter remains before lock destruction.
void generic_spin_queue_assert_quiescent(struct GenericSpinQueue* queue){
  assert(queue != NULL,
    "generic_spin_queue_assert_quiescent: queue is NULL.\n");

  // Every queue operation increments before exchanging into the CLH tail.
  // Therefore active==0 proves there is no owner, contender, or caller in the
  // post-unlock window. The external owner contract separately forbids a new
  // operation from starting after this snapshot.
  int active = __atomic_load_n(&queue->active_operations);
  bool owner_clear = queue->spinlock.owner == NULL;
  bool tail_idle = queue->spinlock.tail != NULL &&
    !queue->spinlock.tail->locked;
  if (active != 0 || !owner_clear || !tail_idle){
    int args[4] = {(int)queue, active, (int)queue->spinlock.owner,
      (int)queue->spinlock.tail};
    say("| queue: generic spin teardown rejected queue=0x%X active=%d owner=0x%X tail=0x%X\n",
      args);
    panic("generic_spin_queue_destroy: owner must stop and join every queue operation before destruction.\n");
  }
}

// Require that no payload remains before destroying the queue lock.
void generic_spin_queue_assert_empty(struct GenericSpinQueue* queue){
  assert(queue != NULL, "generic_spin_queue_assert_empty: queue is NULL.\n");

  // Generic elements remain owned by the queue's caller. Destroy has no
  // payload callback, so accepting a live element would erase the caller's
  // only ownership chain. External quiescence permits this unlocked snapshot.
  int size = __atomic_load_n(&queue->size);
  if (size != 0 || queue->head != NULL || queue->tail != NULL){
    int args[4] = {(int)queue, size, (int)queue->head, (int)queue->tail};
    say("| queue: generic_spin_queue_destroy rejected queue=0x%X size=%d head=0x%X tail=0x%X\n",
      args);
    panic("generic_spin_queue_destroy: owner must drain every payload before destruction.\n");
  }
}

// Release an externally quiescent, empty generic queue's CLH state.
void generic_spin_queue_destroy(struct GenericSpinQueue* queue){
  assert(queue != NULL, "generic_spin_queue_destroy: queue is NULL.\n");
  generic_spin_queue_assert_quiescent(queue);
  generic_spin_queue_assert_empty(queue);

  clh_lock_destroy(&queue->spinlock);
  queue->head = NULL;
  queue->tail = NULL;
  __atomic_store_n(&queue->size, 0);
}

// Append one detached generic element under the CLH lock.
void generic_spin_queue_add(struct GenericSpinQueue* queue, struct GenericQueueElement* data){
  assert(data != NULL, "generic_spin_queue_add: data is NULL.\n");
  __atomic_fetch_add(&queue->active_operations, 1);
  clh_lock_acquire(&queue->spinlock);

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

  clh_lock_release(&queue->spinlock);
  __atomic_fetch_add(&queue->active_operations, -1);
}

// Remove and detach the oldest generic element under the CLH lock.
struct GenericQueueElement* generic_spin_queue_remove(struct GenericSpinQueue* queue){
  assert(queue != NULL, "generic_spin_queue_remove: queue is NULL.\n");
  __atomic_fetch_add(&queue->active_operations, 1);
  clh_lock_acquire(&queue->spinlock);

  if (queue->head == NULL){
    clh_lock_release(&queue->spinlock);
    __atomic_fetch_add(&queue->active_operations, -1);
    return NULL;
  }

  struct GenericQueueElement* node = queue->head;
  queue->head = node->next;
  node->next = NULL;

  if (queue->head == NULL){
    queue->tail = NULL;
  }

  __atomic_fetch_add(&queue->size, -1);

  clh_lock_release(&queue->spinlock);
  __atomic_fetch_add(&queue->active_operations, -1);

  return node;
}

// Detach the complete generic FIFO under the CLH lock.
struct GenericQueueElement* generic_spin_queue_remove_all(struct GenericSpinQueue* queue){
  assert(queue != NULL, "generic_spin_queue_remove_all: queue is NULL.\n");
  __atomic_fetch_add(&queue->active_operations, 1);
  clh_lock_acquire(&queue->spinlock);

  struct GenericQueueElement* head = queue->head;
  queue->head = NULL;
  queue->tail = NULL;
  __atomic_store_n(&queue->size, 0);

  clh_lock_release(&queue->spinlock);
  __atomic_fetch_add(&queue->active_operations, -1);

  return head;
}

// Read the number of generic elements while participating in teardown tracking.
unsigned generic_spin_queue_size(struct GenericSpinQueue* queue){
  assert(queue != NULL, "generic_spin_queue_size: queue is NULL.\n");
  __atomic_fetch_add(&queue->active_operations, 1);
  unsigned size = __atomic_load_n(&queue->size);
  __atomic_fetch_add(&queue->active_operations, -1);
  return size;
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

// Return the number of occupied slots in the circular buffer.
unsigned ringbuf_size(struct RingBuf* rb){
  if (rb->head >= rb->tail){
    return rb->head - rb->tail;
  } else {
    return rb->capacity - (rb->tail - rb->head);
  }
}

// Free a ring buffer's storage and reset its indices.
void ringbuf_destroy(struct RingBuf* rb){
  // free dynamically allocated buffer
  free(rb->buf);
  rb->buf = NULL;
  rb->capacity = 0;
  rb->head = 0;
  rb->tail = 0;
}

// Destroy and free a heap-allocated ring buffer.
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
  if (p == 0) {
    // Don't add 0 keys.
    return true;
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
