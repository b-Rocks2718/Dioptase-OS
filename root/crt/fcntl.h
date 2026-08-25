#ifndef FCNTL_H
#define FCNTL_H

/*
 * Dioptase keeps the original path-only creating `open` ABI. Lookup-only
 * callers use the separate `open_existing` trap so an unused flag argument
 * cannot silently change legacy binaries' creation behavior.
 */
#define O_RDONLY 0

int open(char* pathname);
int open_existing(char* pathname);

#endif // FCNTL_H
