
extern int halt(void);

extern int putchar(int c);

extern int get_core_id(void);

extern int wakeup_core(int core_num);

extern int __atomic_exchange_n(int *ptr, int val);
