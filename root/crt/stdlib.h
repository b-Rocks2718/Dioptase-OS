#ifndef STDLIB_H
#define STDLIB_H

#include "stddef.h"

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

// Return suitably aligned storage, or NULL when the rounded request exceeds
// the total configured user-heap capacity. Exhaustion by live allocations or
// fragmentation retains this CRT's fatal diagnostic policy.
void* malloc(unsigned size);
// If the rounded growth request exceeds total configured capacity, return NULL
// and leave the original allocation unchanged.
void* realloc(void* p, unsigned size);
void free(void* p);
unsigned exit(int status);

#endif // STDLIB_H
