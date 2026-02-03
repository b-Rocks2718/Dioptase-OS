#include "interrupts.h"
#include "print.h"
#include "machine.h"

// Purpose: perform per-core interrupt setup.
// Inputs: none.
// Outputs: none.
// Preconditions: kernel mode; core ID register is readable.
// Postconditions: interrupt subsystem is ready for ISR entry.
// Invariants: PIT handler saves state on the interrupted stack (no ISA register).
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
void interrupts_init(void){
  (void)get_core_id();
}

void spurious_interrupt_handler(void){
  // Purpose: report unexpected interrupt/exception state and halt.
  // Inputs: none (entered via spurious IVT entry).
  // Outputs: diagnostic prints; does not return.
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
