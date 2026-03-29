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

// return a pointer to the TCB for the currently running thread on this core
// Precondition: interrupts or preemption are disabled, or the current thread is pinned to this core
struct TCB* get_current_tcb() {
  return get_per_core()->current_thread;
}
