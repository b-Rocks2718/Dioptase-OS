#ifndef PRINT_H
#define PRINT_H

#include "constants.h"
#include "atomic.h"

extern struct SpinLock print_lock;

// print the number n to the console in decimal
// returns the number of characters printed
unsigned print_signed(int n);

unsigned print_unsigned(unsigned n);

unsigned print_hex(unsigned n, bool uppercase);

unsigned puts(char* str);

// simple printf implementation supporting %d, %u, %x, %X
// accepts an array instead of variadic arguments
unsigned printf(char* fmt, int* arr);

// printf with locking
unsigned say(char* fmt, int* arr);

#endif // PRINT_H