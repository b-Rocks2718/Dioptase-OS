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

struct TCB* pit_handler(unsigned* save_area){
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

  // save caller saved registers
  tcb->r1 = save_area[0];
  tcb->r2 = save_area[1];
  tcb->r3 = save_area[2];
  tcb->r4 = save_area[3];
  tcb->r5 = save_area[4];
  tcb->r6 = save_area[5];
  tcb->r7 = save_area[6];
  tcb->r8 = save_area[7];
  tcb->r9 = save_area[8];
  tcb->r10 = save_area[9];
  tcb->r11 = save_area[10];
  tcb->r12 = save_area[11];
  tcb->r13 = save_area[12];
  tcb->r14 = save_area[13];
  tcb->r15 = save_area[14];
  tcb->r16 = save_area[15];
  tcb->r17 = save_area[16];
  tcb->r18 = save_area[17];
  tcb->r19 = save_area[18];

  tcb->epc = save_area[19];
  tcb->efg = save_area[20];
  tcb->ksp = save_area[21];

  tcb->int_bp = save_area[22];
  tcb->int_ra = save_area[23];

  if (tcb != per_core->idle_thread) {
    unsigned was = disable_interrupts();
    block(was, add_tcb, tcb);
  }

  imr = get_imr();
  assert((imr & 0x80000000) == 0, "interrupts enabled in PIT handler.\n");

  per_core = get_per_core();
  assert(per_core->current_thread == tcb, "current thread changed unexpectedly in PIT handler.\n");

  assert(tcb != NULL, "current thread is NULL in PIT handler.\n");

  return tcb;
}

void pit_init(unsigned hertz){
  // register pit handler
  register_handler(pit_handler_, PIT_IVT_ENTRY);

  // configure pit device
  unsigned cycles_per_tick = CLOCK_FREQ / hertz;
  *PIT_ADDR = cycles_per_tick;
}
