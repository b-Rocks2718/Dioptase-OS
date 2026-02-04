#ifndef PIT_H
#define PIT_H

#include "TCB.h"

extern unsigned current_jiffies;

void pit_init(unsigned hertz);

// Handle PIT interrupts; save_area points to the interrupted thread's stack save area.
void pit_handler(void);

extern void pit_handler_(void);

extern void mark_pit_handled(void);

#endif // PIT_H
