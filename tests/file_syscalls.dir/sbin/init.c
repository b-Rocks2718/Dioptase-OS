#include "../../../crt/sys.h"

/*
 * file_syscalls guest:
 * - validate relative chdir/open/read/write/close behavior through the user
 *   trap wrappers
 * - verify dup() shares the underlying descriptor offset and survives closing
 *   the original fd
 * - verify seek() rejects negative results instead of storing an invalid file
 *   offset
 */

int main(void){
  char buf[8];
  char y = 'Y';

  test_syscall(chdir("./files"));

  int fd = open("hello.txt");
  test_syscall(fd >= 0);
  test_syscall(chdir("hello.txt"));

  test_syscall(read(fd, buf, 1));
  test_syscall(buf[0]);

  int dupfd = dup(fd);
  test_syscall(dupfd >= 0);

  test_syscall(read(dupfd, buf, 1));
  test_syscall(buf[0]);

  test_syscall(seek(dupfd, -2, SEEK_END));
  test_syscall(read(fd, buf, 2));
  test_syscall(buf[0]);
  test_syscall(buf[1]);

  test_syscall(seek(fd, -8, SEEK_CUR));
  test_syscall(seek(fd, 0, SEEK_CUR));

  test_syscall(close(fd));
  test_syscall(read(dupfd, buf, 1));
  test_syscall(close(dupfd));

  fd = open("hello.txt");
  test_syscall(fd >= 0);
  test_syscall(write(fd, &y, 1));
  test_syscall(seek(fd, 0, SEEK_SET));
  test_syscall(read(fd, buf, 1));
  test_syscall(buf[0]);

  test_syscall(play_audio_file(STDOUT));
  test_syscall(close(fd));
  test_syscall(close(fd));

  yield();
  test_syscall(1);

  return 0;
}
