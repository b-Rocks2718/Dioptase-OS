#ifndef SYS_WAIT_H
#define SYS_WAIT_H

#include "types.h"

int wait_child(int pid);

#define WIFEXITED(status) 1
#define WEXITSTATUS(status) (status)

#endif // SYS_WAIT_H
