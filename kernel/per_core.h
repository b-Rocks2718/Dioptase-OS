#ifndef PER_CORE_H
#define PER_CORE_H

#include "constants.h"
#include "TCB.h"
#include "queue.h"

// Be super careful using these!!!
// Weird behavior will happen if preemption occurs
// during a call to these functions

struct TCB;

struct PerCore {
  struct TCB idle_thread;
  struct TCB* current_thread;
  struct Queue ready_queue;
  struct SpinQueue pinned_queue;
  struct SleepQueue sleep_queue;
};

extern struct PerCore per_core_data[MAX_CORES];

struct PerCore* get_per_core(void);

struct TCB* get_current_tcb();

struct TCB* get_idle_tcb();

struct Queue* get_ready_queue();

#endif // PER_CORE_H
