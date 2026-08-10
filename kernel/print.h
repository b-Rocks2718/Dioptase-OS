#ifndef PRINT_H
#define PRINT_H

#include "constants.h"

extern short text_tiles[42]; // size is ignored, just there for compiler

/*
 * Serialize one counted buffer to the kernel console.
 *
 * The buffer is emitted as one console transaction, so another core cannot
 * interleave output or concurrently update the shared cursor, scroll, or text
 * color state. This function executes in kernel mode, may be called with
 * interrupts either enabled or disabled, and restores the caller's exact IMR
 * and preemption states before returning. A same-core interrupt diagnostic may
 * nest without deadlocking; when it interrupts an existing VGA owner, its text
 * is routed to UART so it cannot touch partially updated VGA state. It does not
 * retain `buffer`.
 */
unsigned console_write(char* buffer, unsigned count);

/*
 * Change the color used by subsequent console writes. The color update is
 * ordered with console_write(), say(), printf(), puts(), and putchar().
 */
void console_set_text_color(int color);

// Return a serialized snapshot of the color used by subsequent output.
int console_get_text_color(void);

/*
 * Serialized access to the kernel-owned tile scale and signed 16-bit scroll
 * MMIO registers. The move operations serialize the complete MMIO
 * read/modify/write with console scrolling, including same-core interrupt
 * diagnostics. Each function restores the caller's exact IMR and preemption
 * state. Display-mutating accessors run in ordinary kernel/trap context; only
 * console output is supported from interrupt context.
 */
void console_set_tile_scale(int scale);
void console_set_tile_vscroll(int scroll);
void console_set_tile_hscroll(int scroll);
void console_move_tile_vscroll(int delta);
void console_move_tile_hscroll(int delta);

// Print one character as one serialized console transaction.
void putchar(char c);

/*
 * Print a single character directly to UART, ignoring CONFIG.use_vga.
 * This intentionally bypasses console serialization so panic diagnostics can
 * still make progress if the failing core already held the console lock.
 */
void putchar_uart(char c);

// Print one character with a specific RGB332 color as one transaction.
void putchar_color(char c, int color);

// Print n as one serialized signed-decimal transaction; return its length.
unsigned print_signed(int n);

// Panic-safe direct UART signed-decimal output; ignores CONFIG.use_vga.
unsigned print_signed_uart(int n);

// Print n as one serialized unsigned-decimal transaction; return its length.
unsigned print_unsigned(unsigned n);

// Panic-safe direct UART unsigned-decimal output; ignores CONFIG.use_vga.
unsigned print_unsigned_uart(unsigned n);

// Print n as one serialized hexadecimal transaction; return its length.
unsigned print_hex(unsigned n, bool uppercase);

// Panic-safe direct UART hexadecimal output; ignores CONFIG.use_vga.
unsigned print_hex_uart(unsigned n, bool uppercase);

// Print a NUL-terminated string as one serialized console transaction.
unsigned puts(char* str);

// Panic-safe direct UART output; intentionally not serialized.
unsigned puts_uart(char* str);

// simple printf implementation supporting %d, %u, %x, %X, %s, %c, %%
// accepts an array because the compiler does not yet support variadic functions
// array can contain integers and string pointers
// serializes the complete formatted message with other console writers
unsigned printf(char* fmt, void* arr);

// simple printf implementation supporting %d, %u, %x, %X, %s, %c, %%
// accepts an array because the compiler does not yet support variadic functions
// array can contain integers and string pointers
// serializes the complete formatted UART message; ignores CONFIG.use_vga
unsigned printf_uart(char* fmt, void* arr);

// simple printf implementation supporting %d, %u, %x, %X, %s, %c, %%
// accepts an array because the compiler does not yet support variadic functions
// array can contain integers and string pointers
// acquires print_lock for serialized output
unsigned say(char* fmt, void* arr);

// simple printf implementation supporting %d, %u, %x, %X, %s, %c, %%
// accepts an array because the compiler does not yet support variadic functions
// array can contain integers and string pointers
// acquires print_lock for serialized output
// ignores CONFIG.use_vga
unsigned say_uart(char* fmt, void* arr);

// simple printf implementation supporting %d, %u, %x, %X, %s, %c, %%
// accepts an array because the compiler does not yet support variadic functions
// array can contain integers and string pointers
// acquires print_lock for serialized output and allows specifying text color
unsigned say_color(char* fmt, void* arr, int color);

// load text mode tiles and initialize VGA text mode
void vga_text_init(void);

// clear the screen of all text characters and reset scroll/cursor state
void clear_screen(void);

/*
 * Replace the complete tile framebuffer with transparent entries as one bulk
 * console transaction. The long MMIO loop runs with the caller's original IMR.
 * This does not reset the text cursor; callers normally use it when handing
 * visible output to the pixel framebuffer. It is not an interrupt-context API.
 */
void console_make_tiles_transparent(void);

// set the current tileset to the text mode tileset and clear the screen
void load_text_tiles(void);

// set the current tileset to the text mode tileset with the given text and background colors,
// then clear the screen
void load_text_tiles_colored(short text_color, short bg_color);

bool isnum(char c);

#endif // PRINT_H
