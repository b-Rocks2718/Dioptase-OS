#ifndef MACHINE_H
#define MACHINE_H

#include "constants.h"

extern void shutdown(void);

extern void sleep(void);

void pause(void);

extern void putchar(char c);

extern unsigned get_core_id(void);

extern unsigned get_cr0(void);

extern unsigned get_imr(void);

extern unsigned get_pc(void);

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

// Return the current interrupt status register (ISR) value.
extern unsigned get_isr(void);

// Return the current interrupt mask register (IMR) value.
extern unsigned get_imr(void);

// Return the exception program counter (EPC) value.
extern unsigned get_epc(void);

// Return the exception flags (EFG) value.
extern unsigned get_efg(void);

// Return the TLB miss address register (cr7) value.
extern unsigned get_tlb_addr(void);

extern void wakeup_core(int core_num);

extern void wakeup_all(void);

extern int __atomic_exchange_n(int *ptr, int val);

extern int __atomic_fetch_add(int* ptr, int val);

extern int __atomic_load_n(int* ptr);

extern void __atomic_store_n(int* ptr, int val);

struct TCB;

extern void context_switch(struct TCB* me, struct TCB* next, void (*func)(void *), void *arg, struct TCB** cur_thread);

// Restore a fully-saved thread context from an interrupt handler.
// Purpose: resume the selected thread via rfi without re-saving the current state.
// Inputs: next thread TCB pointer (state already saved by PIT).
// Outputs: does not return; transfers control to the thread.
// Preconditions: executing in interrupt context with interrupts masked.
extern void context_switch_from_isr(struct TCB* next);

#endif // MACHINE_H
