#ifndef TCB_H
#define TCB_H

#include "constants.h"

struct Fun {
  void (*func)(void *);
  void *arg;
};

struct TCB {

  unsigned r1; // offset 0
  unsigned r2; // offset 4
  unsigned r3; // offset 8
  unsigned r4; // offset 12
  unsigned r5; // offset 16
  unsigned r6; // offset 20
  unsigned r7; // offset 24
  unsigned r8; // offset 28
  unsigned r9; // offset 32
  unsigned r10; // offset 36
  unsigned r11; // offset 40
  unsigned r12; // offset 44
  unsigned r13; // offset 48
  unsigned r14; // offset 52
  unsigned r15; // offset 56
  unsigned r16; // offset 60
  unsigned r17; // offset 64
  unsigned r18; // offset 68
  unsigned r19; // offset 72
  unsigned r20; // offset 76
  unsigned r21; // offset 80
  unsigned r22; // offset 84
  unsigned r23; // offset 88
  unsigned r24; // offset 92
  unsigned r25; // offset 96
  unsigned r26; // offset 100
  unsigned r27; // offset 104
  unsigned r28; // offset 108

  unsigned sp;  // offset 112
  unsigned bp;  // offset 116

  unsigned flags; // offset 120
  unsigned ret_addr; // offset 124
  unsigned psr;      // offset 128
  unsigned imr;      // offset 132

  unsigned *stack;
  struct Fun *thread_fun;

  struct TCB* next;
};

#endif // TCB_H
