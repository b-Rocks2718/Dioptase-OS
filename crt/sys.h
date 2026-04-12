#ifndef SYS_H
#define SYS_H

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

void test_syscall_list(int num, int* args);

#endif
