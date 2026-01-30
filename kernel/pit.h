#ifndef PIT_H
#define PIT_H

extern unsigned jiffies;

void pit_init(unsigned hertz);

void pit_handler(void);

extern void pit_handler_(void);

extern void mark_pit_handled(void);

#endif // PIT_H

