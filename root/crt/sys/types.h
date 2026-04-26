#ifndef SYS_TYPES_H
#define SYS_TYPES_H

#include "../stddef.h"

/*
 * Bootstrap compatibility note:
 * bcc still rejects typedef, so keep these conventional names as macro aliases
 * that match the current 32-bit userland ABI.
 */
#define off_t int
#define pid_t int
#define ssize_t int

#endif // SYS_TYPES_H
