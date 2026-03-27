#ifndef PRINT_H
#define PRINT_H

#include "constants.h"
#include "atomic.h"

extern struct PreemptSpinLock print_lock;

extern int text_color; // Note: access only when holding print_lock

extern short text_tiles[42]; // size is ignored, just there for compiler

// print a single character to the console
void putchar(char c);

// print a character with a specific color
// color is in the format 0bRRRGGGBB
// scrolls the screen if we run out of room
void putchar_color(char c, int color);

bool isnum(char c);

// print the number n to the console as a signed decimal
// returns the number of characters printed
unsigned print_signed(int n);

// print the number n to the console as an unsigned decimal
// returns the number of characters printed
unsigned print_unsigned(unsigned n);

// print the number n to the console as a hexadecimal
// returns the number of characters printed
unsigned print_hex(unsigned n, bool uppercase);

// print a NUL-terminated string
unsigned puts(char* str);

// simple printf implementation supporting %d, %u, %x, %X, %s, %%
// accepts an array because the compiler does not yet support variadic functions
// array can contain integers and string pointers
// does not acquire print_lock, 
// so output can be interleaved if other threads are printing
// to avoid this, call say() instead of printf()
unsigned printf(char* fmt, void* arr);

// simple printf implementation supporting %d, %u, %x, %X, %s, %%
// accepts an array because the compiler does not yet support variadic functions
// array can contain integers and string pointers
// acquires print_lock for serialized output
unsigned say(char* fmt, void* arr);

// simple printf implementation supporting %d, %u, %x, %X, %s, %%
// accepts an array because the compiler does not yet support variadic functions
// array can contain integers and string pointers
// acquires print_lock for serialized output and allows specifying text color
unsigned say_color(char* fmt, void* arr, int color);

// load text mode tiles and initialize VGA text mode
void vga_text_init(void);

// clear the screen of all text characters and reset scroll/cursor state
void clear_screen(void);

// set the current tileset to the text mode tileset and clear the screen
void load_text_tiles(void);

// set the current tileset to the text mode tileset with the given text and background colors, 
// then clear the screen
void load_text_tiles_colored(short text_color, short bg_color);

#endif // PRINT_H