#ifndef PRINT_H
#define PRINT_H

#include "constants.h"

void putchar(char c);

unsigned puts(char* str);

unsigned printf(char* fmt, void* arr);

unsigned print_signed(int n);

unsigned print_unsigned(unsigned n);

unsigned print_hex(unsigned n, bool uppercase);

#endif // PRINT_H