#ifndef SYS_H
#define SYS_H

// initialize syscalls
void sys_init(void);

unsigned jump_to_user(unsigned entry, unsigned stack);

#endif // SYS_H