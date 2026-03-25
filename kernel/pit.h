#ifndef PIT_H
#define PIT_H

#include "TCB.h"

// number of jiffies since boot; incremented by PIT handler on each timer interrupt
extern unsigned current_jiffies;

// Initialize the PIT to generate interrupts at the specified frequency in hertz
void pit_init(unsigned hertz);

// Handle PIT interrupts and perform preemptive scheduling
void pit_handler(void);

// asm wrapper for C pit_handler; defined in pit.s
// saves caller-saved registers and preserves interrupted CPU state
extern void pit_handler_(void);

// Mark the current PIT interrupt as handled, allowing the PIT to send more interrupts
extern void mark_pit_handled(void);

#endif // PIT_H
