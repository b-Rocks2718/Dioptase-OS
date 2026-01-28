#ifndef MACHINE_H
#define MACHINE_H

extern void halt(void);

extern void putchar(char c);

extern int get_core_id(void);

extern int get_cr0(void);

extern void wakeup_core(int core_num);

extern void wakeup_all(void);

extern int __atomic_exchange_n(int *ptr, int val);

extern int __atomic_fetch_add(int* ptr, int val);

#endif // MACHINE_H
