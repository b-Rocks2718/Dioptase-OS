#ifndef STDLIB_H
#define STDLIB_H

#include "stddef.h"

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void* malloc(unsigned size);
void* realloc(void* p, unsigned size);
void free(void* p);
unsigned exit(int status);

#endif // STDLIB_H
