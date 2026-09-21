/*
 * Exercises syscall user-pointer validation:
 * - low kernel/physical aliases must be rejected instead of copied through
 * - a failed read must not advance the file descriptor offset
 */
#include "../../../root/crt/sys.h"
#include "../../user_test.h"

#define BAD_LOW_PTR ((void*)0x1000)

int main(void){ /* Verify syscalls reject invalid user pointers. */
  char buf[2];
  char out = 'Z';

  int fd = open("data.txt");
  user_test_expect_eq("open invalid-pointer read fixture", fd >= 0, 1);

  user_test_expect_eq("read(fd, BAD_LOW_PTR, 1)", read(fd, BAD_LOW_PTR, 1), -1);
  user_test_expect_eq("read(fd, buf, 1)", read(fd, buf, 1), 1);
  user_test_expect_eq("failed read preserved file offset", buf[0], 'a');

  close(fd);

  fd = open("data.txt");
  user_test_expect_eq("write(fd, BAD_LOW_PTR, 1)", write(fd, BAD_LOW_PTR, 1), -1);
  user_test_expect_eq("write(fd, &out, 1)", write(fd, &out, 1), 1);
  close(fd);

  fd = open("data.txt");
  user_test_expect_eq("read(fd, buf, 1)", read(fd, buf, 1), 1);
  user_test_expect_eq("valid write after rejected write", buf[0], out);
  close(fd);

  user_test_expect_eq("open((char*)BAD_LOW_PTR)", open((char*)BAD_LOW_PTR), -1);
  user_test_expect_eq("pipe((int*)BAD_LOW_PTR)", pipe((int*)BAD_LOW_PTR), -1);

  return 0;
}
