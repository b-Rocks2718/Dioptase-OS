#include "queue.h"
#include "heap.h"
#include "atomic.h"
#include "machine.h"
#include "constants.h"

struct SpinQueue* create_spin_queue(){
  struct SpinQueue* queue = malloc(sizeof(struct SpinQueue));

  queue->head = NULL;
  queue->tail = NULL;
  queue->spinlock.the_lock = 0 ;
  queue->size = 0;

  return queue;
}

void spin_queue_add(struct SpinQueue* queue, struct TCB* data){
  spin_lock_get(&queue->spinlock);

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
  spin_lock_get(&queue->spinlock);

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
  spin_lock_get(&queue->spinlock);

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
  spin_lock_get(&queue->spinlock);

  struct TCB* head = queue->head;

  spin_lock_release(&queue->spinlock);

  return head;
}
