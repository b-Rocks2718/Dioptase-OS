#ifndef MACHINE_H
#define MACHINE_H

#include "constants.h"
#include "TCB.h"

// Permanantly halt this core
// in the emulator, and core running this will end the emulation
extern void shutdown(void);

// Put this core to sleep until an interrupt wakes it up
// Ensure interrupts are enabled before sleeping, or the core may never wake up
extern void mode_sleep(void);

// Put this core to sleep if it is safe to do so, otherwise just return
void pause(void);

// return the core's id (0 - 3)
// read from the cores cr9 register, which is read-only
extern unsigned get_core_id(void);

// return the core's cr0 value, which determines whether we're in user mode or kernel mode
// 0 = user mode, nonzero = kernel mode
// cr0 is a counter that is incremented on traps/exceptions/interrupts, and decremented when they exit
extern unsigned get_cr0(void);

// get the core's current process id (cr1)
extern unsigned* get_pid(void);

// set the core's current process id (cr1), return the old pid
extern unsigned set_pid(unsigned val);

// Return the core's interrupt status register (cr2) value
extern unsigned get_isr(void);

// Return the current interrupt mask register (cr3) value
extern unsigned get_imr(void);

// Return the exception program counter (cr4) value
extern unsigned get_epc(void);

// Return the exception flags (cr6) value
extern unsigned get_efg(void);

// Return the TLB miss address register (cr7) value
extern unsigned get_tlb_addr(void);

// Read the TLB entry for the given virtual address, returning the physical address it maps to
// returns 0 if there is no TLB entry for the given virtual address
extern unsigned tlb_read(void* vaddr);

// Write a TLB entry for the given virtual address to physical address mapping, 
// with the given permissions
extern void tlb_write(unsigned entry, unsigned paddr);

// Invalidate the TLB entry for the given virtual address
extern void tlb_invalidate(void* vaddr);

// Invalidate the TLB entry for the given virtual address
extern void tlb_invalidate_other(unsigned pid, void* vaddr);

// invalidate all tlb entries on this core
extern void tlb_flush(void);

// Wake up the core with the given core_num (0 - 3) by sending an IPI
extern void wakeup_core(int core_num);

#endif // MACHINE_H
