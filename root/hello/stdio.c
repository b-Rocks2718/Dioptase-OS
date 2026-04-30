#include "stdio.h"

#define DECIMAL_BASE 10u
#define HEX_BASE 16u
#define MAX_INT_DEC_DIGITS 10 
#define MAX_SIGNED_DEC_CHARS 11
#define MAX_UNSIGNED_HEX_DIGITS 8 

extern int write(int fd, void* buf, unsigned count);

// write() may complete only part of the requested block, so keep issuing calls
// until the full buffer has been consumed or the kernel reports failure.
static void write_fd_all(int fd, char* buf, unsigned count){
  int written;

  while (count != 0){
    written = write(fd, buf, count);
    if (written <= 0){
      return;
    }

    buf += (unsigned)written;
    count -= (unsigned)written;
  }
}

unsigned fdputs(int fd, char* str){
  char* start = str;
  unsigned count = 0;
  while(*str){
    ++str;
    ++count;
  }
  write_fd_all(fd, start, count);
  return count;
}

unsigned puts(char* str){
  return fdputs(STDOUT, str);
}
