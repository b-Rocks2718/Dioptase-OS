#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "TCB.h"

extern unsigned TIME_QUANTUM[MLFQ_LEVELS];
extern unsigned MLFQ_BOOST_INTERVAL;
extern unsigned REBALANCE_INTERVAL;

// initialize scheduler structures; should only be called by threads_init
void scheduler_init(void);

// add a thread to the global ready queue, or if it's pinned, to its core's pinned queue
void global_queue_add(void* tcb);

// remove a thread from the global ready queue
struct TCB* global_queue_remove(void);

// add a thread to the core-local ready queue
void local_queue_add(void* tcb);

// remove a thread from the core-local ready queue
struct TCB* local_queue_remove(void);

// choose the next thread to run on this core, or return NULL to stay idle
struct TCB* schedule_next_thread(void);

void set_priority(enum ThreadPriority priority);

#endif // SCHEDULER_H
