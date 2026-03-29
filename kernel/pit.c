#include "pit.h"
#include "interrupts.h"
#include "print.h"
#include "machine.h"
#include "atomic.h"
#include "TCB.h"
#include "threads.h"
#include "per_core.h"
#include "debug.h"
#include "scheduler.h"

static unsigned* PIT_ADDR = (unsigned*)0x7FE5804;
static unsigned PIT_CLOCK_FREQ = 100000000; // 100MHz clock
static void* PIT_IVT_ENTRY = (void*)0x3C0;

// number of jiffies since boot; incremented by PIT handler on each timer interrupt
unsigned current_jiffies = 0;

// Handle PIT interrupts and perform preemptive scheduling
void pit_handler(void){
  mark_pit_handled(); // clear ISR bit so we don't get duplicate interrupts

  int me = get_core_id();

  int imr = get_imr();
  assert((imr & 0x80000000) == 0, "interrupts enabled in PIT handler.\n");

  if (me == 0){
    // core 0 is responsible for incrementing jiffies
    current_jiffies++;

    // core 0 is responsible for setting mlfq_boost_pending and rebalance_pending on all cores
    // If each core set the value itself, there is a race where it checks current_jiffies
    // before core 0 has incremented it.
    // Each core will clear the value itself
    if (current_jiffies % MLFQ_BOOST_INTERVAL == 0){
      for (int core = 0; core < MAX_CORES; core++){
        __atomic_store_n(&per_core_data[core].mlfq_boost_pending, true);
      }
    }

    if (current_jiffies % REBALANCE_INTERVAL == 0){
      for (int core = 0; core < MAX_CORES; core++){
        __atomic_store_n(&per_core_data[core].rebalance_pending, true);
      }
    }
  }

  struct PerCore* per_core = get_per_core();

  struct TCB* tcb = per_core->current_thread;
  assert(tcb != NULL, "current thread is NULL in PIT handler.\n");

  if (tcb != &per_core->idle_thread){
    tcb->remaining_quantum--;
    if (tcb->can_preempt && tcb->remaining_quantum <= 0){
      // if we aren't idle and can be preempted, and used up our quantum
      // block and add ourselves back to the core-local ready queue 
      // and demote our MLFQ level
      if (tcb->mlfq_level < LEVEL_TWO){
        tcb->mlfq_level++;
      }

      // reset quantum
      tcb->remaining_quantum = TIME_QUANTUM[tcb->mlfq_level];

      unsigned was = get_imr();

      int args[2] = {(int)tcb, (int)&per_core->idle_thread};

      block(was, local_queue_add, tcb, true);
    }
  }

  imr = get_imr();
  assert((imr & 0x80000000) == 0, "interrupts enabled in PIT handler.\n");

  per_core = get_per_core();
  assert(per_core->current_thread == tcb, "current thread changed unexpectedly in PIT handler.\n");

  assert(tcb != NULL, "current thread is NULL in PIT handler.\n");
}

// Initialize the PIT to generate interrupts at the specified frequency in hertz
void pit_init(unsigned hertz){
  // register pit handler
  register_handler(pit_handler_, PIT_IVT_ENTRY);

  // configure pit device
  unsigned cycles_per_tick = PIT_CLOCK_FREQ / hertz;
  *PIT_ADDR = cycles_per_tick;
}
