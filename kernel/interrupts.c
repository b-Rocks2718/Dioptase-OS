#include "interrupts.h"
#include "print.h"
#include "machine.h"

// report unexpected interrupt/exception state and halt
// filled into all unused IVT entries to catch spurious interrupts and exceptions
void spurious_interrupt_handler(void){
  int me = get_core_id();
  unsigned isr = get_isr();
  unsigned imr = get_imr();
  unsigned epc = get_epc();
  unsigned efg = get_efg();
  unsigned tlb = get_tlb_addr();
  unsigned psr = get_cr0();
  int args[7] = { me, (int)isr, (int)imr, (int)epc, (int)efg, (int)tlb, (int)psr };
  say("| Spurious interrupt core=%d isr=0x%X imr=0x%X epc=0x%X efg=0x%X tlb=0x%X psr=0x%X\n", args);
}
