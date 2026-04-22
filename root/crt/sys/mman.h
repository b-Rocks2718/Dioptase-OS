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

void* mmap(unsigned size, int fd, unsigned offset, unsigned flags);

#endif // SYS_MMAN_H
