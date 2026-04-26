#ifndef UNISTD_H
#define UNISTD_H

#include "stddef.h"

#define STDIN 0
#define STDOUT 1
#define STDERR 2

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/*
 * The bootstrap userland still exposes `seek` rather than POSIX `lseek`.
 * Keep the familiar whence macros here because the syscall and stdio shims
 * both depend on them.
 */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int read(int fd, void* buf, unsigned count);
int write(int fd, void* buf, unsigned count);
int close(int fd);

int fork(void);
int execv(char* pathname, int argc, char** argv);
int chdir(char* path);
int pipe(int* fds);
int dup(int fd);

void sleep(unsigned jiffies);
void yield(void);

int getdents(int fd, char* buffer, unsigned buffer_size);
char* getcwd(char* buffer, unsigned buffer_size);
int readlink(char* path, char* buffer, unsigned buffer_size);

int seek(int fd, int offset, int whence);
int fd_bytes_available(int fd);
int truncate(int fd, unsigned size);

#endif // UNISTD_H
