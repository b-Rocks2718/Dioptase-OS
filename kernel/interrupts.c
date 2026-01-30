#include "interrupts.h"
#include "print.h"
#include "machine.h"

void interrupts_init(void){
  unsigned me = get_core_id();
  // set isp to top of core's interrupt stack area
  set_isp(0xF0000 - (me * 0x4000)); 
}

void spurious_interrupt_handler(void){
  int me = get_core_id();
  say("| Spurious interrupt received for core %d\n", &me);
}
