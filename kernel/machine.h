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
// cr0 is a counter that is incremented on syscalls/exceptions/interrupts, and decremented when they exit
extern unsigned get_cr0(void);

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

// Return the caller return address using the current stack frame.
// Purpose: debug aid to locate the call site of a kernel helper.
// Inputs: none.
// Outputs: return address of the current function's caller.
// Preconditions: standard ABI prologue in effect; bp points to saved frame.
// CPU state assumptions: kernel mode; interrupts may be enabled.
extern unsigned get_return_address(void);

// Return the caller's caller return address (two frames up).
// Purpose: debug aid to locate higher-level call sites.
// Inputs: none.
// Outputs: return address of the current function's caller's caller.
// Preconditions: standard ABI prologue in effect for the two callers.
// CPU state assumptions: kernel mode; interrupts may be enabled.
extern unsigned get_caller_return_address(void);

// Atomically set *ptr to val, and return the old value of *ptr
extern int __atomic_exchange_n(int *ptr, int val);

// Atomically add val to *ptr, and return the old value of *ptr
extern int __atomic_fetch_add(int* ptr, int val);

// Atomically load value stored in *ptr
extern int __atomic_load_n(int* ptr);

// Atomically store val into *ptr
extern void __atomic_store_n(int* ptr, int val);

// Wake up the core with the given core_num (0 - 3) by sending an IPI
extern void wakeup_core(int core_num);

// Perform a context switch from the current thread (me) to the next thread (next)
// Run func(arg) in the context of the next thread before switching to it
// Needs pointer to current_thread entry of the core's PerCore struct
// Assumes interrupts are disabled when this is called, and will re-enable them in the new thread's context
// run_with_interrupts determines whether to re-enable interrupts before calling the callback function, or after
// Assumes callback doesn't modify the 'next' TCB 
extern void context_switch(struct TCB* me, struct TCB* next, void (*func)(void *), void *arg, 
  struct TCB** cur_thread, int was, bool run_with_interrupts);

#endif // MACHINE_H
