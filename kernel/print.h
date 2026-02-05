#ifndef PRINT_H
#define PRINT_H

#include "constants.h"
#include "atomic.h"

extern struct SpinLock print_lock;

extern int text_color; // Note: access only when holding print_lock

extern short text_tiles[42]; // size is ignored, just there for compiler

void putchar(char c);

void putchar_color(char c, int color);

bool isnum(char c);

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

unsigned say_color(char* fmt, int* arr, int color);

void clear_screen(void);

void vga_text_init(void);

void load_text_tiles(void);

#endif // PRINT_H