#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "TCB.h"

extern unsigned TIME_QUANTUM[MLFQ_LEVELS];
extern unsigned MLFQ_BOOST_INTERVAL;
extern unsigned REBALANCE_INTERVAL;

// initialize scheduler structures; should only be called by threads_init
void scheduler_init(void);

// destroy scheduler queue locks after all cores have reached shutdown
void scheduler_destroy(void);

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

// Interrupt-safe wakeup path for ISRs that must remain bounded-time. The TCB is
// appended in O(1), without taking a spin lock, to this core's deferred
// interrupt-wake queue; the idle thread later routes it through
// scheduler_wake_thread() from schedule_next_thread(), outside interrupt context.
void scheduler_wake_thread_from_interrupt(struct TCB* tcb);

// choose the next thread to run on this core, or return NULL to stay idle
struct TCB* schedule_next_thread(void);

// Set the current thread's static priority for its next ready-queue entry.
void set_priority(enum ThreadPriority priority);

#endif // SCHEDULER_H
