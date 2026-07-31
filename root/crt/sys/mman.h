#ifndef SYS_MMAN_H
#define SYS_MMAN_H

#include "types.h"

/*
 * Dioptase keeps protection and sharing bits in the single flags word passed
 * to mmap(). Anonymous mappings are still requested with fd == MAP_ANON.
 */
#define PROT_NONE  0x00
#define PROT_READ  0x04
#define PROT_WRITE 0x08
#define PROT_EXEC  0x10

#define MAP_PRIVATE   0x00
#define MAP_SHARED    0x01
#define MAP_ANON      -1
#define MAP_ANONYMOUS MAP_ANON

/*
 * Dioptase's documented syscall API uses MMAP_* names. Keep the conventional
 * aliases above for source compatibility, while exposing the canonical names
 * expected by existing user programs and docs/syscalls.md.
 */
#define MMAP_READ    0x04
#define MMAP_WRITE   0x08
#define MMAP_EXEC    0x10
#define MMAP_PRIVATE 0x00
#define MMAP_SHARED  0x01
#define MMAP_ANON    -1

void* mmap(unsigned size, int fd, unsigned offset, unsigned flags);

#endif // SYS_MMAN_H
