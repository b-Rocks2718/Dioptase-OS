#include "per_core.h"
#include "machine.h"
#include "constants.h"
#include "interrupts.h"

struct PerCore per_core_data[MAX_CORES];

// return a pointer to the PerCore struct for the current core
// Precondition: interrupts or preemption are disabled, or the current thread is pinned to this core
//
// The address is built from shifts and adds rather than &per_core_data[me]:
// sizeof(struct PerCore) is not a power of two, so bcc lowers that index to a
// call to the software smul routine (about 80 instructions), and this runs in
// every get_current_tcb(), lock acquire and release, trap, and TLB miss.
// Each set bit of the core ID adds the matching power-of-two multiple of the
// struct size; core IDs are below MAX_CORES, so this takes at most two steps.
struct PerCore* get_per_core(void){
  unsigned me = get_core_id();
  char* slot = (char*)per_core_data;
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
