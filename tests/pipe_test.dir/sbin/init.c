#include "../../../crt/print.h"
#include "../../../crt/sys.h"
#include "../../../crt/constants.h"

// Read one start value, print its Collatz sequence, and free the list.
int main(void) {
  int pipe_fds[2];
  if (pipe(pipe_fds) < 0){
    puts("***pipe creation failed\n");
    return -1;
  }

  puts("***writing to pipe\n");
  write(pipe_fds[1], "***hello from init\n", 19);
  
  char buf[32];
  
  puts("***reading from pipe\n");
  int n = read(pipe_fds[0], buf, 19);
  if (n > 0){
    buf[n] = '\0';
    puts(buf);
  }

  int dup_write_fd = dup(pipe_fds[1]);
  puts("***duplicated write end of pipe\n");

  int dup_read_fd = dup(pipe_fds[0]);
  puts("***duplicated read end of pipe\n");

  write(dup_write_fd, "***hello again from init\n", 25);

  puts("***reading from pipe again\n");
  n = read(dup_read_fd, buf, 25);
  if (n > 0){
    buf[n] = '\0';
    puts(buf);
  }

  puts("***done\n");

  return 67;
}
