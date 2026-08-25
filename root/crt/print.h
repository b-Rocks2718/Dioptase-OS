#ifndef PRINT_H
#define PRINT_H

#include "stdio.h"
#include "stdbool.h"

/*
 * These descriptor/string helpers return their exact emitted byte count after
 * complete success, or -1 if any write fails or stops making progress. Output
 * committed before a later failure remains visible. Unlike ISO C puts(), this
 * small CRT's puts() preserves its historical behavior and adds no newline.
 */
int fdputs(int fd, char* str);

int printf(char* fmt, void* arr);

// Purpose: Emit formatted text to an arbitrary file descriptor without
// variadics. The caller passes every argument as a 32-bit slot in arr.
// Supported formats intentionally match the subset used by Dioptase userland:
// %d, %u, %x, %X, %s, %.*s, %c, and %%.
// Returns the exact byte count on complete success or -1 after any output
// failure. One ignored l/z modifier is accepted before an integer conversion;
// it still consumes one 32-bit argument slot and is not 64-bit long support.
int fdprintf(int fd, char* fmt, void* arr);

int print_signed(int n);

int print_unsigned(unsigned n);

int print_hex(unsigned n, bool uppercase);

#endif // PRINT_H
