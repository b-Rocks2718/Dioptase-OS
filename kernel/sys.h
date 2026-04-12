#ifndef SYS_H
#define SYS_H

#include "ext.h"

enum TrapCode {
  TRAP_CODE_EXIT = 0,
  TRAP_CODE_TEST_SYSCALL = 1
};

// set up IVT with trap handler entry point
void trap_init(void);

// Enter user mode through rfe
unsigned jump_to_user(unsigned entry, unsigned stack);

// run a user program given a node representing its ELF file
// consumes the node, so the caller cannot use it after calling this function
int run_user_program(struct Node* prog_node);

extern void trap_handler_(void);

#endif // SYS_H
