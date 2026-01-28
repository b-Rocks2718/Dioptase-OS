#ifndef PIT_H
#define PIT_H

extern unsigned jiffies;

void pit_init(unsigned hertz);

extern void mark_pit_handled(void);

#endif // PIT_H

