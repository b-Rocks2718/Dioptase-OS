#ifndef SYS_STAT_H
#define SYS_STAT_H

#include "types.h"

/*
 * The current kernel only exposes the size field through the bootstrap file
 * metadata paths used by userland tools. Add fields here only when the kernel
 * interface starts providing them.
 */
struct stat {
  int st_size;
};

#endif // SYS_STAT_H
