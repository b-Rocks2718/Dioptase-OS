#ifndef MACHINE_H
#define MACHINE_H

extern void shutdown(void);

extern void sleep(void);

extern void putchar(char c);

extern unsigned get_core_id(void);

extern unsigned get_cr0(void);

extern unsigned get_pc(void);

extern void wakeup_core(int core_num);

extern void wakeup_all(void);

extern int __atomic_exchange_n(int *ptr, int val);

extern int __atomic_fetch_add(int* ptr, int val);

extern int __atomic_load_n(int* ptr);

extern void __atomic_store_n(int* ptr, int val);

struct TCB;

extern void context_switch(struct TCB* me, struct TCB* next, void (*func)(void *), void *arg);

#endif // MACHINE_H
