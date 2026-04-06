#ifndef TCB_H
#define TCB_H

#include "constants.h"
//#include "vmem.h"

// function and argument for a thread to run
struct Fun {
  void (*func)(void *);
  void *arg;
};

// The core a thread is pinned to, if any
enum CoreAffinity {
  CORE_0 = 0,
  CORE_1 = 1,
  CORE_2 = 2,
  CORE_3 = 3,
  ANY_CORE = 0xFFFFFFFF
};

#define PRIORITY_LEVELS 3
#define MLFQ_LEVELS 3

// priority levels for threads, used for scheduling decisions
enum ThreadPriority {
  LOW_PRIORITY = 0,
  NORMAL_PRIORITY = 1,
  HIGH_PRIORITY = 2,
};

enum MLFQ_LEVEL {
  LEVEL_ZERO = 0,
  LEVEL_ONE = 1,
  LEVEL_TWO = 2
};

// Thread Control Block
// One per thread, stores all info about the thread including its context for switching
struct TCB {
  unsigned r20; // offset 0
  unsigned r21; // offset 4
  unsigned r22; // offset 8
  unsigned r23; // offset 12
  unsigned r24; // offset 16
  unsigned r25; // offset 20
  unsigned r26; // offset 24
  unsigned r27; // offset 28
  unsigned r28; // offset 32

  unsigned sp;  // offset 36
  unsigned bp;  // offset 40

  unsigned flags; // offset 44
  unsigned ret_addr; // offset 48
  unsigned psr;      // offset 52
  unsigned imr;      // offset 56
  unsigned pid;      // offset 60

  unsigned* stack;
  struct Fun* thread_fun;

  bool can_preempt;
  enum CoreAffinity core_affinity;
  enum ThreadPriority priority;
  enum MLFQ_LEVEL mlfq_level;
  int remaining_quantum;
  unsigned wakeup_jiffies;

  //struct VME* vme_list;

  struct TCB* next;
};

#endif // TCB_H
