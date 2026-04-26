#ifndef SYS_H
#define SYS_H

#include "fcntl.h"
#include "unistd.h"
#include "sys/mman.h"
#include "sys/wait.h"

/*
 * Deprecated compatibility aliases for code that still includes sys.h and uses
 * the older combined MMAP_* names. New code should use PROT_* and MAP_*.
 */
#define MMAP_ANON   -1
#define MMAP_NONE   0x00
#define MMAP_SHARED 0x01
#define MMAP_READ   0x04
#define MMAP_WRITE  0x08
#define MMAP_EXEC   0x10

#define MAX_PATH 1024

#define DIOPTASE_PRIORITY_LOW 0
#define DIOPTASE_PRIORITY_NORMAL 1
#define DIOPTASE_PRIORITY_HIGH 2

unsigned exit(int status);

unsigned test_syscall(int arg);

unsigned get_current_jiffies(void);

unsigned getkey(void);

void set_tile_scale(unsigned scale);

void set_vscroll(unsigned value);

void set_hscroll(unsigned value);

void load_text_tiles(void);

void clear_screen(void);

short* get_tilemap(void);

short* get_tile_fb(void);

unsigned get_vga_status(void);

unsigned get_vga_frame_counter(void);

void sleep(unsigned jiffies);

int sem_open(int count);

int sem_up(int sem);

int sem_down(int sem);

int sem_close(int sem);

int play_audio_file(int fd);

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

short* get_spritemap(void);

int kill(int child);

unsigned* get_synth_audio(void);

int request_priority(int priority);

void test_syscall_list(int num, int* args);

#endif // SYS_H
