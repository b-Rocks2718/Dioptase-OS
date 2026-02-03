#ifndef QUEUE_H
#define QUEUE_H

#include "TCB.h"
#include "atomic.h"

struct SpinQueue {
  struct TCB* head;
  struct TCB* tail;
  struct SpinLock spinlock;
  int size;
};

struct Queue {
  struct TCB* head;
  struct TCB* tail;
  int size;
};

void spin_queue_init(struct SpinQueue* queue);

void spin_queue_add(struct SpinQueue* queue, struct TCB* data);

struct TCB* spin_queue_remove(struct SpinQueue* queue);

struct TCB* spin_queue_remove_all(struct SpinQueue* queue);

unsigned spin_queue_size(struct SpinQueue* queue);

struct TCB* spin_queue_peek(struct SpinQueue* queue);


void queue_init(struct Queue* queue);

void queue_add(struct Queue* queue, struct TCB* data);

struct TCB* queue_remove(struct Queue* queue);

struct TCB* queue_remove_all(struct Queue* queue);

unsigned queue_size(struct Queue* queue);

struct TCB* queue_peek(struct Queue* queue);

#endif // QUEUE_H
