#include "pit.h"
#include "interrupts.h"
#include "print.h"
#include "machine.h"
#include "atomic.h"
#include "TCB.h"
#include "threads.h"
#include "per_core.h"
#include "debug.h"

static unsigned* PIT_ADDR = (unsigned*)0x7FE5804;
static unsigned CLOCK_FREQ = 100000000; // 100MHz clock
static void* PIT_IVT_ENTRY = (void*)0x3C0;

unsigned jiffies = 0;

// Purpose: handle PIT interrupts and perform preemptive scheduling.
// Inputs: save_area points to the interrupted thread's stack save area.
// Outputs: returns the TCB to resume; does not return if the core halts.
// Preconditions: kernel mode; interrupts disabled on entry; save_area valid.
// Postconditions: current thread state saved to TCB; ready queue updated.
// Invariants: save_area layout matches pit.s; per-core current_thread stable
// across the handler except when block() switches contexts.
void pit_handler(void){
  mark_pit_handled();

  int me = get_core_id();

  int imr = get_imr();
  assert((imr & 0x80000000) == 0, "interrupts enabled in PIT handler.\n");

  if (me == 0){
    jiffies++;
  }

  struct PerCore* per_core = get_per_core();

  struct TCB* tcb = per_core->current_thread;
  assert(tcb != NULL, "current thread is NULL in PIT handler.\n");

  if (tcb != per_core->idle_thread) {
    unsigned was = disable_interrupts();
    block(was, add_tcb, tcb);
  }

  imr = get_imr();
  assert((imr & 0x80000000) == 0, "interrupts enabled in PIT handler.\n");

  per_core = get_per_core();
  assert(per_core->current_thread == tcb, "current thread changed unexpectedly in PIT handler.\n");

  assert(tcb != NULL, "current thread is NULL in PIT handler.\n");
}

void pit_init(unsigned hertz){
  // register pit handler
  register_handler(pit_handler_, PIT_IVT_ENTRY);

  // configure pit device
  unsigned cycles_per_tick = CLOCK_FREQ / hertz;
  *PIT_ADDR = cycles_per_tick;
}
