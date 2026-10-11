#include "per_core.h"
#include "machine.h"
#include "constants.h"
#include "interrupts.h"

struct PerCore per_core_data[MAX_CORES];

// Find this core's state without the software multiply emitted for array indexing.
// Kernel callers must prevent migration while obtaining and using this pointer:
// disable interrupts or preemption, or pin the calling thread to this core.
// CID is below MAX_CORES; the physical kernel array bypasses the MMU. This
// function changes no interrupt, preemption, or shared synchronization state.
// Each set CID bit adds the corresponding power-of-two multiple of the struct
// size, so the current four-core maximum needs at most two loop iterations.
struct PerCore* get_per_core(void){
  unsigned me = get_core_id();
  char* slot = (char*)&per_core_data;
  unsigned stride = sizeof(struct PerCore);
  while (me != 0){
    if (me & 1){
      slot += stride;
    }
    stride <<= 1;
    me >>= 1;
  }
  return (struct PerCore*)slot;
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
