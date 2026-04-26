/*
 * user_truncate guest:
 * - verify truncate(fd, size) shrinks one open regular file descriptor
 * - verify descriptor offsets stay unchanged across truncate, so reads from an
 *   old offset past the new EOF now clamp to zero bytes
 * - verify truncate persists the smaller EOF across reopen and rejects both
 *   growth requests and directory descriptors
 * - verify regrowth through the current write() path zero-fills the gap after
 *   the new EOF even though truncate intentionally does not reclaim blocks
 */

#include "../../../root/crt/sys.h"

int main(void){
  char buf[8];
  char z = 'Z';

  int fd = open("shrink.txt");
  test_syscall(fd >= 0);
  test_syscall(write(fd, "ABCDE", 5));
  test_syscall(seek(fd, 4, SEEK_SET));
  test_syscall(truncate(fd, 3));
  test_syscall(read(fd, buf, 1));
  test_syscall(close(fd));

  fd = open("shrink.txt");
  test_syscall(fd >= 0);
  test_syscall(read(fd, buf, 5));
  test_syscall(buf[0]);
  test_syscall(buf[1]);
  test_syscall(buf[2]);
  test_syscall(truncate(fd, 4));
  test_syscall(seek(fd, 4, SEEK_SET));
  test_syscall(write(fd, &z, 1));
  test_syscall(seek(fd, 0, SEEK_SET));
  test_syscall(read(fd, buf, 5));
  test_syscall(buf[0]);
  test_syscall(buf[1]);
  test_syscall(buf[2]);
  test_syscall(buf[3]);
  test_syscall(buf[4]);
  test_syscall(close(fd));

  int dir_fd = open("/");
  test_syscall(dir_fd >= 0);
  test_syscall(truncate(dir_fd, 0));
  test_syscall(close(dir_fd));

  return 0;
}
