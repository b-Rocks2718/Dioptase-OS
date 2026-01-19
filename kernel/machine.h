
extern int halt(void);

extern int putchar(char c);

extern int get_core_id(void);

extern int get_cr0(void);

extern int wakeup_core(int core_num);

extern int __atomic_exchange_n(int *ptr, int val);

int __atomic_fetch_add(int* ptr, int val);
