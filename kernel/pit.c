#include "pit.h"
#include "interrupts.h"
#include "print.h"
#include "machine.h"
#include "atomic.h"
#include "TCB.h"
#include "threads.h"
#include "per_core.h"

static unsigned* PIT_ADDR = (unsigned*)0x7FE5804;
static unsigned CLOCK_FREQ = 100000000; // 100MHz clock
static void* PIT_IVT_ENTRY = (void*)0x3C0;

unsigned jiffies = 0;

void pit_handler(void){
  mark_pit_handled();

  int me = get_core_id();

  if (me == 0){
    jiffies++;
  }

  struct PerCore* per_core = get_per_core();

  struct TCB* tcb = per_core->current_thread;
  //if (tcb->can_preempt) {
  //  block(false, add_tcb, tcb);
  //}
}

void pit_init(unsigned hertz){
  // register pit handler
  register_handler(pit_handler_, PIT_IVT_ENTRY);

  // configure pit device
  unsigned cycles_per_tick = CLOCK_FREQ / hertz;
  *PIT_ADDR = cycles_per_tick;
}
