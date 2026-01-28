#ifndef INTERRUPTS_H
#define INTERRUPTS_H

// returns current isr value
unsigned clear_isr(void);

// restores imr to prev, returns value that was in imr
unsigned restore_interrupts(unsigned prev);

// returns current imr value
unsigned disable_interrupts(void);

void register_handler(void* func, void* ivt_entry);

void register_spurious_handlers(void);

#endif // INTERRUPTS_H