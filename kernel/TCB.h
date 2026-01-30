#ifndef TCB_H
#define TCB_H

#include "constants.h"

struct Fun {
  void (*func)(void *);
  void *arg;
};

struct TCB {

  unsigned r20; // offset 0
  unsigned r21; // offset 4
  unsigned r22; // offset 8
  unsigned r23; // offset 12
  unsigned r24; // offset 16
  unsigned r25; // offset 20
  unsigned r26; // offset 24
  unsigned r27; // offset 28

  unsigned sp;  // offset 32
  unsigned bp;  // offset 36

  unsigned flags; // offset 40
  unsigned ret_addr; // offset 44

  unsigned *stack;
  struct Fun *thread_fun;

  bool can_preempt;

  struct TCB* next;
};

#endif // TCB_H
