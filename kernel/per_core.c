#include "per_core.h"
#include "machine.h"
#include "constants.h"
#include "interrupts.h"

struct PerCore per_core_data[MAX_CORES];

// return a pointer to the PerCore struct for the current core
// Precondition: interrupts or preemption are disabled, or the current thread is pinned to this core
struct PerCore* get_per_core(void){
  int me = get_core_id();
  return &per_core_data[me];
}

// Return the TCB running on this core. The core-ID read and the
// current_thread load happen with interrupts disabled, so a PIT preemption
// cannot migrate this thread between them and return another core's TCB.
// Safe from any kernel context; the caller's exact IMR is restored.
struct TCB* get_current_tcb(void) {
  unsigned was = interrupts_disable();
  struct TCB* current = get_per_core()->current_thread;
  interrupts_restore(was);
  return current;
}
