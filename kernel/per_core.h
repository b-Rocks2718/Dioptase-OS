#ifndef PER_CORE_H
#define PER_CORE_H

struct TCB;

struct PerCore {
  struct TCB* idle_thread;
  struct TCB* current_thread;
};

struct PerCore* get_per_core(void);

struct TCB* get_current_tcb();

struct TCB* get_idle_tcb();

#endif // PER_CORE_H
