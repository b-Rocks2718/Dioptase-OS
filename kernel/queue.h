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

struct SpinQueue* create_spin_queue();

void spin_queue_add(struct SpinQueue* queue, struct TCB* data);

struct TCB* spin_queue_remove(struct SpinQueue* queue);

struct TCB* spin_queue_remove_all(struct SpinQueue* queue);

unsigned spin_queue_size(struct SpinQueue* queue);

struct TCB* spin_queue_peek(struct SpinQueue* queue);

#endif // QUEUE_H
