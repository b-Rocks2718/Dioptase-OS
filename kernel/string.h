#ifndef STRING_H
#define STRING_H

#include "constants.h"

unsigned strlen(char* str);

bool streq(char* str1, char* str2);

bool strneq(char* str1, char* str2, unsigned n);

// Copies up to `n` bytes from `src`. If `src` ends earlier, this helper writes
// exactly one trailing NUL and leaves the rest of `dest` unchanged.
char* strncpy(char* dest, char* src, unsigned n);

// Copies `n` raw bytes from `src` into `dest`. Source and destination must not
// overlap because this helper performs a simple forward byte copy.
void* memcpy(void* dest, void* src, unsigned n);

// Fills `n` bytes at `dest` with the low byte of `c`.
void* memset(void* dest, int c, unsigned n);

#endif // STRING_H
