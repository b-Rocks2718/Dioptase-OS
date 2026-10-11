#ifndef SYS_H
#define SYS_H

#include "fcntl.h"
#include "unistd.h"
#include "sys/mman.h"
#include "sys/wait.h"

#define MAX_PATH 1024

#define DIOPTASE_PRIORITY_LOW 0
#define DIOPTASE_PRIORITY_NORMAL 1
#define DIOPTASE_PRIORITY_HIGH 2

// Signals [0, MAX_MASKABLE_SIGNAL) can register handlers and be masked.
#define SIGNAL_HELLO 0
#define SIGNAL_TERMINATE 1

#define MAX_MASKABLE_SIGNAL 16

// can register handlers for, but cannot mask
#define SIGNAL_SEG 16
#define SIGNAL_ILL 17
#define SIGNAL_ALGN 18

// cannot mask or register a handler for
#define SIGNAL_KILL 31

unsigned exit(int status);

unsigned test_syscall(int arg);

unsigned get_current_jiffies(void);

unsigned getkey(void);

// Return the oldest queued PS/2 mouse event word, or 0 if none is pending.
// Non-blocking. Decode the word with the macros in mouse.h.
int getmouse(void);

void set_tile_scale(unsigned scale);

void set_vscroll(unsigned value);

void set_hscroll(unsigned value);

void load_text_tiles(void);

void clear_screen(void);

// Map the tilemap MMIO into this process. Each cell is a device register.
volatile short* get_tilemap(void);

// Map the tile framebuffer MMIO into this process.
volatile short* get_tile_fb(void);

// Map the pixel framebuffer into this process. The window is the page-aligned
// prefix of the 320x240 buffer that stops before the PS/2, UART, PIT, SD, and
// audio registers (0x25000 bytes: rows 0..235 are complete, stride 320).
// Returns the same pointer on repeat calls, or (short*)-1 if no virtual
// range is available.
volatile short* get_pixel_fb(void);

// Write the pixel-scale register and return 0. Hardware displays each pixel
// at 2**(n+1), so 0 fills 640x480 from the 320x240 buffer and 1 shows the
// top-left 160x120 of that buffer at 4x. Claims the foreground display.
int set_pixel_scale(unsigned scale);

// Ask the terminal to deliver raw PS/2 events on stdin: each event is the
// same 16-bit getkey() word, little-endian, including releases (bit 8).
// `enabled` must be 0 (cooked characters) or 1 (raw). Returns 0, or -1 with
// errno EINVAL. Exec clears the mode. Forked children start cooked.
int set_keyboard_raw(int enabled);

// Return 1 when the live foreground child has raw keyboard mode set, else 0.
int foreground_keyboard_raw(void);

unsigned get_vga_status(void);

unsigned get_vga_frame_counter(void);

void sleep(unsigned jiffies);

int sem_open(int count);

int sem_up(int sem);

int sem_down(int sem);

int sem_close(int sem);

/*
 * Audio output device (docs/syscalls.md, "Audio"). The device plays signed
 * 16-bit little-endian mono PCM at 25 kHz from a ring of about 0.33 s. One
 * process owns it at a time; exit or exec releases it automatically. Every
 * call is non-blocking: keep calling audio_write() to keep the ring fed, or the
 * device plays silence until it is refilled. root/crt/wav.h wraps this for WAV
 * files.
 */

// Claim the audio device, reset to empty and stopped. Returns 0, or -1 with
// errno EBUSY if another process (or this one) already owns it.
int audio_open(void);

// Queue up to `bytes` (even) of PCM and start playback. Returns how many bytes
// were accepted, which is less than `bytes` (possibly 0) when the ring is
// full; resubmit the rest later. Returns -1 with errno EBADF (not the owner),
// EINVAL (odd byte count), or EFAULT (unreadable buffer).
int audio_write(void* pcm, unsigned bytes);

// Return how many queued bytes have not been played yet, or -1 with errno
// EBADF if the caller does not own the device. Playback position in bytes is
// (total bytes accepted) - audio_buffered().
int audio_buffered(void);

// Stop immediately, discard queued audio, and release the device. Returns 0,
// or -1 with errno EBADF if the caller does not own it. To let queued audio
// finish first, wait until audio_buffered() reaches 0.
int audio_close(void);

void set_text_color(int color);

void yield(void);

int move_vscroll(int delta);

int move_hscroll(int delta);

int mkdir(char* path);

int rmdir(char* path);

int unlink(char* path);

int set_sprite_scale(unsigned sprite_num, unsigned scale);

int set_sprite_coords(unsigned sprite_num, unsigned x, unsigned y);

int load_text_tiles_colored(unsigned fg_color, unsigned bg_color);

// Map the spritemap MMIO into this process. Each cell is a device register.
volatile short* get_spritemap(void);

int signal_child(int child, int signal);

int request_priority(int priority);

int set_foreground_child(int child);

int signal_foreground(int signal);

int register_handler(int signal, void* handler);

int sigreturn(int rc);

int mask_signal(int signal);

int unmask_signal(int signal);

void test_syscall_list(int num, int* args);

#endif // SYS_H
