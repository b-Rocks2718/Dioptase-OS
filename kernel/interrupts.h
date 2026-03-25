#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include "constants.h"

// disables interrupts and returns previous imr value
extern unsigned interrupts_disable(void);

// restores imr to prev, returns value that was in imr
extern unsigned interrupts_restore(unsigned prev);

// register the given handler function for the given IVT entry
void register_handler(void* func, void* ivt_entry);

// register spurious_interrupt_handler for all IVT entries to catch unexpected interrupts and exceptions
extern void register_spurious_handlers(void);

#endif // INTERRUPTS_H
