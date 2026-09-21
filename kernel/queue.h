#ifndef QUEUE_H
#define QUEUE_H

#include "atomic.h"

// TCB queues:

struct TCB;

// FIFO queue protected by an internal spin lock
struct SpinQueue {
  struct TCB* head;
  struct TCB* tail;
  struct CLHLock spinlock;
  int size;
};

// FIFO queue for callers that already provide synchronization
struct Queue {
  struct TCB* head;
  struct TCB* tail;
  int size;
};

// SleepQueue is a single-owner queue of threads sleeping until wakeup_jiffies.
// Invariant: entries are ordered by 32-bit modular time within the supported
// INT_MAX-tick horizon, and equal deadlines retain FIFO order. There is no
// internal lock; production callers rely on the per-core sleep queue ownership
// and interrupt discipline described by the scheduler, while tests may call
// the explicit-time helper directly.
struct SleepQueue {
  struct TCB* head;
  int size;
};

// Generic queue:
// Invariant: elements that go in to the queue must have 'next' pointer as first member.
struct GenericQueueElement {
  struct GenericQueueElement* next;
};

// Store caller-owned elements in an unlocked FIFO list.
struct GenericQueue {
  struct GenericQueueElement* head;
  struct GenericQueueElement* tail;
  int size;
};

// GenericQueue protected by an internal spin lock
struct GenericSpinQueue {
  struct GenericQueueElement* head;
  struct GenericQueueElement* tail;
  struct CLHLock spinlock;
  int size;
  // Diagnostic lifetime reference spanning every public operation from before
  // its first CLH exchange through its final queue access/unlock.
  int active_operations;
};

// Circular buffer that leaves one slot empty to distinguish full from empty
struct RingBuf {
  void** buf;
  unsigned capacity;
  unsigned head;
  unsigned tail;
};

#define KEYBUF_CAPACITY 64

// Circular buffer that leaves one slot empty to distinguish full from empty
// SPSC queue
struct KeyBuf {
  short buf[KEYBUF_CAPACITY];
  unsigned head;
  unsigned tail;
};

// initialize an empty spin-locked FIFO queue
void spin_queue_init(struct SpinQueue* queue);

// Destroy an empty queue's internal spin lock after all users have stopped.
// Panics rather than silently abandoning queued TCB ownership.
void spin_queue_destroy(struct SpinQueue* queue);

// append a TCB to the tail of the spin queue
void spin_queue_add(struct SpinQueue* queue, struct TCB* data);

// remove and return the head TCB from the spin queue
struct TCB* spin_queue_remove(struct SpinQueue* queue);

// detach and return the entire spin queue
struct TCB* spin_queue_remove_all(struct SpinQueue* queue);

// return the current number of elements in the spin queue
unsigned spin_queue_size(struct SpinQueue* queue);

// return the head TCB without removing it
struct TCB* spin_queue_peek(struct SpinQueue* queue);


// initialize an empty FIFO queue
void queue_init(struct Queue* queue);

// append a TCB to the tail of the queue
void queue_add(struct Queue* queue, struct TCB* data);

// remove and return the head TCB from the queue
struct TCB* queue_remove(struct Queue* queue);

// detach and return the entire queue
struct TCB* queue_remove_all(struct Queue* queue);

// return the current number of elements in the queue
unsigned queue_size(struct Queue* queue);

// return the head TCB without removing it
struct TCB* queue_peek(struct Queue* queue);


// initialize an empty sleep queue
void sleep_queue_init(struct SleepQueue* queue);

// Insert a sleeping thread, keeping the queue sorted across 32-bit jiffy wrap.
// All live deadlines must remain within INT_MAX ticks of the comparison epoch.
// Precondition: caller owns this queue or otherwise excludes concurrent
// sleep_queue_add/remove operations on the same SleepQueue.
// Postcondition: data is linked into the queue with stale next linkage cleared.
void sleep_queue_add(void* args);

// Remove the head thread if its wakeup time has arrived at the provided modular
// time. The provided time and every live deadline must differ by < 2^31 ticks.
// Precondition: caller owns this queue or otherwise excludes concurrent
// sleep_queue_add/remove operations on the same SleepQueue.
// Postcondition: if non-NULL is returned, that node is detached and next=NULL;
// if NULL is returned, the queue is unchanged.
struct TCB* sleep_queue_remove_at(struct SleepQueue* queue, unsigned now_jiffies);

// remove the head thread if its wakeup time has arrived according to current_jiffies.
// Same ownership and postconditions as sleep_queue_remove_at().
struct TCB* sleep_queue_remove(struct SleepQueue* queue);

// Detach every sleeper from an externally quiescent single-owner queue.
// Returned nodes retain their list linkage; the queue becomes empty.
struct TCB* sleep_queue_remove_all(struct SleepQueue* queue);

// return the current number of sleeping threads
unsigned sleep_queue_size(struct SleepQueue* queue);


// initialize an empty generic FIFO queue
void generic_queue_init(struct GenericQueue* queue);

// append an element to the tail of the generic queue
void generic_queue_add(struct GenericQueue* queue, struct GenericQueueElement* data);

// remove and return the head element from the generic queue
struct GenericQueueElement* generic_queue_remove(struct GenericQueue* queue);

// detach and return the entire generic queue
struct GenericQueueElement* generic_queue_remove_all(struct GenericQueue* queue);

// return the current number of elements in the generic queue
unsigned generic_queue_size(struct GenericQueue* queue);


// initialize an empty spin-locked generic queue
void generic_spin_queue_init(struct GenericSpinQueue* queue);

// Non-mutating teardown preflight for queue operations/CLH ownership. This
// deliberately does not inspect payload links, allowing composite owners to
// emit their richer size/permit/capacity diagnostics before any destruction.
void generic_spin_queue_assert_quiescent(struct GenericSpinQueue* queue);

// Non-mutating payload preflight for an externally quiescent queue.
void generic_spin_queue_assert_empty(struct GenericSpinQueue* queue);

// Destroy an empty queue's internal spin lock after all users have stopped.
// Panics rather than silently abandoning caller-owned payloads.
void generic_spin_queue_destroy(struct GenericSpinQueue* queue);

// append an element to the tail of the spin-locked generic queue
void generic_spin_queue_add(struct GenericSpinQueue* queue, struct GenericQueueElement* data);

// remove and return the head element from the spin-locked generic queue
struct GenericQueueElement* generic_spin_queue_remove(struct GenericSpinQueue* queue);

// detach and return the entire spin-locked generic queue
struct GenericQueueElement* generic_spin_queue_remove_all(struct GenericSpinQueue* queue);

// return the current number of elements in the spin-locked generic queue
unsigned generic_spin_queue_size(struct GenericSpinQueue* queue);


// allocate storage for a ring buffer with the given capacity
void ringbuf_init(struct RingBuf* rb, unsigned capacity);

// push an element at the front; returns false if the buffer is full
bool ringbuf_add_front(struct RingBuf* rb, void* p);

// push an element at the back; returns false if the buffer is full
bool ringbuf_add_back(struct RingBuf* rb, void* p);

// pop and return the front element, or NULL if empty
void* ringbuf_remove_front(struct RingBuf* rb);

// pop and return the back element, or NULL if empty
void* ringbuf_remove_back(struct RingBuf* rb);

// return the current number of stored elements
unsigned ringbuf_size(struct RingBuf* rb);

// free the ring buffer storage without freeing the RingBuf struct itself
void ringbuf_destroy(struct RingBuf* rb);

// destroy the ring buffer and free the RingBuf struct
void ringbuf_free(struct RingBuf* rb);


// initialize keybuf
void keybuf_init(struct KeyBuf* kb);

// push an element at the front; returns false if the buffer is full
bool keybuf_add(struct KeyBuf* kb, short p);

// pop and return the back element, or 0 if empty
short keybuf_remove(struct KeyBuf* kb);

// return the current number of stored elements
unsigned keybuf_size(struct KeyBuf* kb);

#endif // QUEUE_H
