#ifndef PER_CORE_H
#define PER_CORE_H

#include "constants.h"
#include "TCB.h"
#include "queue.h"

// Stores all core-local data
struct PerCore {
  struct TCB idle_thread; // TCB for the idle thread on this core
  struct TCB* current_thread; // TCB for the currently running thread on this core
  struct Queue ready_queue; // queue of threads ready to run on this core
  struct SpinQueue pinned_queue; // queue of threads pinned to this core
  struct SleepQueue sleep_queue; // queue of threads sleeping on this core
};

extern struct PerCore per_core_data[MAX_CORES];

// return a pointer to the PerCore struct for the current core
// Precondition: interrupts or preemption are disabled, or the current thread is pinned to this core
struct PerCore* get_per_core(void);

// return a pointer to the TCB for the idle thread on this core
// Precondition: interrupts or preemption are disabled, or the current thread is pinned to this core
struct TCB* get_idle_tcb();

// return a pointer to the TCB for the currently running thread on this core
// Precondition: interrupts or preemption are disabled, or the current thread is pinned to this core
struct TCB* get_current_tcb();

// return a pointer to the ready queue for this core
// Precondition: interrupts or preemption are disabled, or the current thread is pinned to this core
struct Queue* get_ready_queue();

// return a pointer to the pinned queue for this core
// Precondition: interrupts or preemption are disabled, or the current thread is pinned to this core
struct SpinQueue* get_pinned_queue();

// return a pointer to the sleep queue for this core
// Precondition: interrupts or preemption are disabled, or the current thread is pinned to this core
struct SleepQueue* get_sleep_queue();

#endif // PER_CORE_H
