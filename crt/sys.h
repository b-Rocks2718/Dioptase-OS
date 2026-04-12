#ifndef SYS_H
#define SYS_H

unsigned exit(int status);

unsigned test_syscall(int arg);

void test_syscall_list(int num, int* args);

#endif