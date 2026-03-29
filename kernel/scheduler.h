#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "TCB.h"

extern unsigned TIME_QUANTUM[MLFQ_LEVELS];
extern unsigned MLFQ_BOOST_INTERVAL;
extern unsigned REBALANCE_INTERVAL;

// initialize scheduler structures; should only be called by threads_init
void scheduler_init(void);

// add a thread to the global ready queues, or if it's pinned, to its core's pinned queue
void global_queue_add(void* tcb);

// remove a thread from the global ready queues using the shared priority/MLFQ policy
struct TCB* global_queue_remove(void);

// add a thread to the core-local ready queue
void local_queue_add(void* tcb);

// remove a thread from the core-local ready queue
struct TCB* local_queue_remove(void);

// Charge one unit of MLFQ budget to a thread that voluntarily yielded.
void scheduler_charge_yield(struct TCB* tcb);

// Make a blocked thread runnable again
void scheduler_wake_thread(struct TCB* tcb);

// Interrupt-safe wakeup path for ISRs that must remain bounded-time
// - same-core and ANY_CORE work is admitted in O(1) to this core's local ready queue
// - remote pinned work is deferred to a current-core queue and routed by the
//   normal scheduler path outside interrupt context
void scheduler_wake_thread_from_interrupt(struct TCB* tcb);

// choose the next thread to run on this core, or return NULL to stay idle
struct TCB* schedule_next_thread(void);

void set_priority(enum ThreadPriority priority);

#endif // SCHEDULER_H
