#ifndef PRINT_H
#define PRINT_H

#include "stdio.h"
#include "stdbool.h"

unsigned fdputs(int fd, char* str);

unsigned printf(char* fmt, void* arr);

// Purpose: Emit formatted text to an arbitrary file descriptor without
// variadics. The caller passes every argument as a 32-bit slot in arr.
// Supported formats intentionally match the subset used by Dioptase userland:
// %d, %u, %ld, %zu, %x, %X, %s, %.*s, %c, and %%.
unsigned fdprintf(int fd, char* fmt, void* arr);

unsigned print_signed(int n);

unsigned print_unsigned(unsigned n);

unsigned print_hex(unsigned n, bool uppercase);

#endif // PRINT_H
