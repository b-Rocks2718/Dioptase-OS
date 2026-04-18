/*
 * Exercises syscall user-pointer validation:
 * - low kernel/physical aliases must be rejected instead of copied through
 * - a failed read must not advance the file descriptor offset
 */
#include "../../../crt/sys.h"

#define BAD_LOW_PTR ((void*)0x1000)

int main(void){
  char buf[2];
  char out = 'Z';

  int fd = open("data.txt");
  test_syscall(fd >= 0);

  test_syscall(read(fd, BAD_LOW_PTR, 1));
  test_syscall(read(fd, buf, 1));
  test_syscall(buf[0]);

  close(fd);

  fd = open("data.txt");
  test_syscall(write(fd, BAD_LOW_PTR, 1));
  test_syscall(write(fd, &out, 1));
  close(fd);

  fd = open("data.txt");
  test_syscall(read(fd, buf, 1));
  test_syscall(buf[0]);
  close(fd);

  test_syscall(open((char*)BAD_LOW_PTR));
  test_syscall(pipe((int*)BAD_LOW_PTR));

  return 0;
}
