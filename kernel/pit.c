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

struct TCB* pit_handler(unsigned* sp){
  mark_pit_handled();

  int me = get_core_id();

  if (me == 0){
    jiffies++;
  }

  struct PerCore* per_core = get_per_core();

  struct TCB* tcb = per_core->current_thread;

  assert(tcb != NULL, "Error: current thread is NULL in PIT handler.\n");

  // save caller saved registers
  tcb->r1 = sp[-1];
  tcb->r2 = sp[-2];
  tcb->r3 = sp[-3];
  tcb->r4 = sp[-4];
  tcb->r5 = sp[-5];
  tcb->r6 = sp[-6];
  tcb->r7 = sp[-7];
  tcb->r8 = sp[-8];
  tcb->r9 = sp[-9];
  tcb->r10 = sp[-10];
  tcb->r11 = sp[-11];
  tcb->r12 = sp[-12];
  tcb->r13 = sp[-13];
  tcb->r14 = sp[-14];
  tcb->r15 = sp[-15];
  tcb->r16 = sp[-16];
  tcb->r17 = sp[-17];
  tcb->r18 = sp[-18];
  tcb->r19 = sp[-19];

  tcb->epc = sp[-20];
  tcb->efg = sp[-21];
  tcb->ksp = sp[-22];

  tcb->int_bp = sp[-23];
  tcb->int_ra = sp[-24];

  if (tcb != per_core->idle_thread) {
    block(false, add_tcb, tcb);
  }

  per_core = get_per_core();
  assert(per_core->current_thread == tcb, "Error: current thread changed unexpectedly in PIT handler.\n");

  return tcb;
}

void pit_init(unsigned hertz){
  // register pit handler
  register_handler(pit_handler_, PIT_IVT_ENTRY);

  // configure pit device
  unsigned cycles_per_tick = CLOCK_FREQ / hertz;
  *PIT_ADDR = cycles_per_tick;
}
