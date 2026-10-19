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

// Ask the terminal for raw (1) or cooked (0) keyboard input on STDIN and wait
// until it has switched; input from before the switch is discarded. Raw input
// is two-byte records: the guest keycode, then TERMINAL_RAW_RELEASE (1) for a
// release or 0 for a press (root/crt/terminal.h). Not a trap: implemented in
// keyboard.c with the terminal's in-band requests (docs/terminal.md). Returns
// 0, or -1 (errno EINVAL for a bad mode) when no terminal answered, for
// example when STDIN is not the terminal's input pipe.
int set_keyboard_raw(int enabled);

void set_tile_scale(unsigned scale);

void set_vscroll(unsigned value);

void set_hscroll(unsigned value);

void load_text_tiles(void);

void clear_screen(void);

// Map the tilemap MMIO into this process. Each cell is a device register.
volatile short* get_tilemap(void);

// Map the tile framebuffer MMIO into this process.
volatile short* get_tile_fb(void);

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

// Map the 320x240 pixel framebuffer (the layer drawn under the tiles; 0x0BGR
// pixels, row-major) into this process. Only pixels 0..75775 are mapped (rows
// 0..235 and the first 256 pixels of row 236): the framebuffer's last 2,048
// bytes share a page with device registers, so they stay kernel-only. Returns
// the same pointer on repeat calls, or (short*)-1 if no virtual range is
// available.
#define PIXEL_FB_USER_PIXELS 75776
volatile short* get_pixel_fb(void);

// Write the pixel-scale register and return 0. Hardware displays each pixel
// at 2**(n+1) screen pixels (docs/mem_map.md), so 0 fills the 640x480 screen
// from the 320x240 buffer and 1 shows its top-left 160x120 at 4x. The
// terminal restores 0 after a foreground program that changed it exits.
int set_pixel_scale(unsigned scale);

// Write both pixel-layer scroll registers and return 0. Framebuffer pixel
// (x, y) appears at screen ((x + sx) mod 320, (y + sy) mod 240): the layer is a
// ring buffer, so a scrolling program redraws only the strip coming into view
// (docs/mem_map.md). The terminal restores (0, 0) after a foreground program
// that changed it exits.
int set_pixel_scroll(int x, int y);

// The last pixel index of the pixel layer plus one (320 * 240).
#define PIXEL_FB_PIXELS 76800

// Copy `count` pixels from `src` into framebuffer pixels first..first+count-1,
// which must lie in [PIXEL_FB_USER_PIXELS, PIXEL_FB_PIXELS): the part of the
// layer get_pixel_fb() cannot map. Returns 0, or -1 with errno EINVAL for a
// range outside that tail or EFAULT for an unreadable `src`.
int write_pixel_tail(unsigned first, short* src, unsigned count);

int signal_child(int child, int signal);

int request_priority(int priority);

// Pin the calling thread to `core`, moving it there before returning, or
// with -1 let it run on any core again. Returns the core the thread runs on
// when the call returns, or -1 if `core` is not a core of this machine. A
// forked child inherits the pin.
int pin_core(int core);

int set_foreground_child(int child);

int signal_foreground(int signal);

int register_handler(int signal, void* handler);

int sigreturn(int rc);

int mask_signal(int signal);

int unmask_signal(int signal);

void test_syscall_list(int num, int* args);

#endif // SYS_H
