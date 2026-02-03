#ifndef PIT_H
#define PIT_H

#include "TCB.h"

extern unsigned jiffies;

void pit_init(unsigned hertz);

struct TCB* pit_handler(unsigned* sp);

extern void pit_handler_(void);

extern void fake_pit_handler(void);

extern void mark_pit_handled(void);

#endif // PIT_H

