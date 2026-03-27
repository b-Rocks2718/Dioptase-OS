#include "per_core.h"
#include "machine.h"
#include "constants.h"

struct PerCore per_core_data[MAX_CORES];

// return a pointer to the PerCore struct for the current core
// Precondition: interrupts or preemption are disabled, or the current thread is pinned to this core
struct PerCore* get_per_core(void){
  int me = get_core_id();
  return &per_core_data[me];
}

// return a pointer to the TCB for the idle thread on this core
// Precondition: interrupts or preemption are disabled, or the current thread is pinned to this core
struct TCB* get_idle_tcb() {
  return &get_per_core()->idle_thread;
}

// return a pointer to the TCB for the currently running thread on this core
// Precondition: interrupts or preemption are disabled, or the current thread is pinned to this core
struct TCB* get_current_tcb() {
  return get_per_core()->current_thread;
}

// return a pointer to the ready queue for this core
// Precondition: interrupts or preemption are disabled, or the current thread is pinned to this core
struct Queue* get_ready_queue() {
  return &get_per_core()->ready_queue;
}

// return a pointer to the pinned queue for this core
// Precondition: interrupts or preemption are disabled, or the current thread is pinned to this core
struct SpinQueue* get_pinned_queue() {
  return &get_per_core()->pinned_queue;
}

// return a pointer to the sleep queue for this core
// Precondition: interrupts or preemption are disabled, or the current thread is pinned to this core
struct SleepQueue* get_sleep_queue() {
  return &get_per_core()->sleep_queue;
}
