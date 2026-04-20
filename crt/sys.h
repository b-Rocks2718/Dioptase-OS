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

#define STDIN 0
#define STDOUT 1
#define STDERR 2

int open(char* pathname);

int read(int fd, void* buf, unsigned count);

int write(int fd, void* buf, unsigned count);

int close(int fd);

int sem_open(int count);

int sem_up(int sem);

int sem_down(int sem);

int sem_close(int sem);

#define MMAP_ANON   -1

#define MMAP_NONE   0x00
#define MMAP_SHARED 0x01
#define MMAP_READ   0x04
#define MMAP_WRITE  0x08
#define MMAP_EXEC   0x10

void* mmap(unsigned size, int fd, unsigned offset, unsigned flags);

int fork(void);

int execv(char *pathname, int argc, char** argv);

int play_audio_file(int fd);

void set_text_color(int color);

int wait_child(int pid);

int chdir(char* path);

int pipe(int* fds);

int dup(int fd);

void yield(void);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int seek(int fd, int offset, int whence);

int move_vscroll(int delta);

int move_hscroll(int delta);

// used for pipes, return -1 if fd is not a pipe
int fd_bytes_available(int fd);

void test_syscall_list(int num, int* args);

#endif
