#ifndef FCNTL_H
#define FCNTL_H

/*
 * The current Dioptase `open` syscall still uses the original path-only ABI.
 * Keep the limited flag surface explicit until the kernel grows a flagful
 * openat/open interface.
 */
#define O_RDONLY 0

int open(char* pathname);

#endif // FCNTL_H
