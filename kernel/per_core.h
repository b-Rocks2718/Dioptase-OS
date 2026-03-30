#ifndef PER_CORE_H
#define PER_CORE_H

#include "constants.h"
#include "TCB.h"
#include "queue.h"

// Stores all core-local data
struct PerCore {
  struct TCB idle_thread;
  struct TCB* current_thread;
  struct Queue ready_queue[PRIORITY_LEVELS][MLFQ_LEVELS];
  struct Queue deferred_interrupt_wake_queue;
  struct SpinQueue pinned_queue;
  struct SleepQueue sleep_queue;
  unsigned scheduler_iters;
  bool mlfq_boost_pending;
  bool rebalance_pending;
  struct KeyBuf keybuf;
};

extern struct PerCore per_core_data[MAX_CORES];

// return a pointer to the PerCore struct for the current core
// Precondition: interrupts or preemption are disabled, or the current thread is pinned to this core
struct PerCore* get_per_core(void);

// return a pointer to the TCB for the currently running thread on this core
// Precondition: interrupts or preemption are disabled, or the current thread is pinned to this core
struct TCB* get_current_tcb();

#endif // PER_CORE_H
