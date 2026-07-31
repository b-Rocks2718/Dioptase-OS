/*
 * file_syscalls guest:
 * - validate relative chdir/open/read/write/close behavior through the user
 *   trap wrappers
 * - verify dup() shares the underlying descriptor offset and survives closing
 *   the original fd
 * - verify seek() rejects negative results instead of storing an invalid file
 *   offset
 * - verify open() can create missing intermediate directories before creating
 *   the final file
 */

#include "../../../root/crt/sys.h"
#include "../../user_test.h"

int main(void){
  char buf[8];
  char y = 'Y';

  user_test_expect_eq("chdir(\"./files\")", chdir("./files"), 0);

  int fd = open("hello.txt");
  user_test_expect_eq("open hello.txt", fd >= 0, 1);
  user_test_expect_eq("chdir(\"hello.txt\")", chdir("hello.txt"), -1);

  user_test_expect_eq("read(fd, buf, 1)", read(fd, buf, 1), 1);
  user_test_expect_eq("hello.txt first byte", buf[0], 'H');

  int dupfd = dup(fd);
  user_test_expect_eq("dupfd >= 0", dupfd >= 0, 1);

  user_test_expect_eq("read(dupfd, buf, 1)", read(dupfd, buf, 1), 1);
  user_test_expect_eq("shared-offset next byte", buf[0], 'e');

  user_test_expect_eq("seek(dupfd, -2, SEEK_END)", seek(dupfd, -2, SEEK_END), 5);
  user_test_expect_eq("read(fd, buf, 2)", read(fd, buf, 2), 2);
  user_test_expect_eq("hello.txt penultimate byte", buf[0], '!');
  user_test_expect_eq("hello.txt final newline", buf[1], '\n');

  user_test_expect_eq("seek(fd, -8, SEEK_CUR)", seek(fd, -8, SEEK_CUR), -1);
  user_test_expect_eq("seek(fd, 0, SEEK_CUR)", seek(fd, 0, SEEK_CUR), 7);

  user_test_expect_eq("close(fd)", close(fd), 0);
  user_test_expect_eq("read(dupfd, buf, 1)", read(dupfd, buf, 1), 0);
  user_test_expect_eq("close(dupfd)", close(dupfd), 0);

  fd = open("hello.txt");
  user_test_expect_eq("reopen hello.txt", fd >= 0, 1);
  user_test_expect_eq("write(fd, &y, 1)", write(fd, &y, 1), 1);
  user_test_expect_eq("seek(fd, 0, SEEK_SET)", seek(fd, 0, SEEK_SET), 0);
  user_test_expect_eq("read(fd, buf, 1)", read(fd, buf, 1), 1);
  user_test_expect_eq("updated hello.txt first byte", buf[0], 'Y');

  user_test_expect_eq("play_audio_file(STDOUT)", play_audio_file(STDOUT), -1);
  user_test_expect_eq("close(fd)", close(fd), 0);
  user_test_expect_eq("close(fd)", close(fd), -1);

  fd = open("generated/./deep/../deep/note.txt");
  user_test_expect_eq("create nested generated note", fd >= 0, 1);
  user_test_expect_eq("write(fd, &y, 1)", write(fd, &y, 1), 1);
  user_test_expect_eq("seek(fd, 0, SEEK_SET)", seek(fd, 0, SEEK_SET), 0);
  user_test_expect_eq("read(fd, buf, 1)", read(fd, buf, 1), 1);
  user_test_expect_eq("nested note written byte", buf[0], 'Y');
  user_test_expect_eq("close(fd)", close(fd), 0);

  user_test_expect_eq("chdir(\"generated/deep\")", chdir("generated/deep"), 0);
  fd = open("note.txt");
  user_test_expect_eq("reopen nested note by basename", fd >= 0, 1);
  user_test_expect_eq("read(fd, buf, 1)", read(fd, buf, 1), 1);
  user_test_expect_eq("reopened nested note byte", buf[0], 'Y');
  user_test_expect_eq("close(fd)", close(fd), 0);

  yield();
  user_test_expect_eq("yield resumed the current process", 1, 1);

  return 0;
}
