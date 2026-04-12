#ifndef SYS_H
#define SYS_H

enum TrapCode {
  TRAP_CODE_EXIT = 0,
};

// set up IVT with trap handler entry point
void trap_init(void);

// Enter user mode through rfe
unsigned jump_to_user(unsigned entry, unsigned stack);

extern void trap_handler_(void);

#endif // SYS_H
