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
#include "../../user_test.h"

int main(void){
  char buf[8];
  char z = 'Z';

  int fd = open("shrink.txt");
  user_test_expect_eq("open shrink fixture", fd >= 0, 1);
  user_test_expect_eq("write(fd, \"ABCDE\", 5)", write(fd, "ABCDE", 5), 5);
  user_test_expect_eq("seek(fd, 4, SEEK_SET)", seek(fd, 4, SEEK_SET), 4);
  user_test_expect_eq("truncate(fd, 3)", truncate(fd, 3), 0);
  user_test_expect_eq("read(fd, buf, 1)", read(fd, buf, 1), 0);
  user_test_expect_eq("close(fd)", close(fd), 0);

  fd = open("shrink.txt");
  user_test_expect_eq("reopen truncated fixture", fd >= 0, 1);
  user_test_expect_eq("read(fd, buf, 5)", read(fd, buf, 5), 3);
  user_test_expect_eq("truncated byte zero", buf[0], 'A');
  user_test_expect_eq("truncated byte one", buf[1], 'B');
  user_test_expect_eq("truncated byte two", buf[2], 'C');
  user_test_expect_eq("truncate(fd, 4)", truncate(fd, 4), -1);
  user_test_expect_eq("seek(fd, 4, SEEK_SET)", seek(fd, 4, SEEK_SET), 4);
  user_test_expect_eq("write(fd, &z, 1)", write(fd, &z, 1), 1);
  user_test_expect_eq("seek(fd, 0, SEEK_SET)", seek(fd, 0, SEEK_SET), 0);
  user_test_expect_eq("read(fd, buf, 5)", read(fd, buf, 5), 5);
  user_test_expect_eq("regrown byte zero", buf[0], 'A');
  user_test_expect_eq("regrown byte one", buf[1], 'B');
  user_test_expect_eq("regrown byte two", buf[2], 'C');
  user_test_expect_eq("zero-filled growth gap", buf[3], 0);
  user_test_expect_eq("regrown final byte", buf[4], 'Z');
  user_test_expect_eq("close(fd)", close(fd), 0);

  int dir_fd = open("/");
  user_test_expect_eq("dir_fd >= 0", dir_fd >= 0, 1);
  user_test_expect_eq("truncate(dir_fd, 0)", truncate(dir_fd, 0), -1);
  user_test_expect_eq("close(dir_fd)", close(dir_fd), 0);

  return 0;
}
