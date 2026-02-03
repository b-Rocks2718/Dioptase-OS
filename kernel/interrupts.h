#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include "constants.h"

// returns current isr value
extern unsigned clear_isr(void);


// restores imr to prev, returns value that was in imr
extern unsigned restore_interrupts(unsigned prev);

// returns current imr value
extern unsigned disable_interrupts(void);

extern void register_spurious_handlers(void);

void register_handler(void* func, void* ivt_entry);

void interrupts_init(void);

#endif // INTERRUPTS_H
