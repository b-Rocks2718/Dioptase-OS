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
 * - canonicalize a valid path with more than 16 components without corrupting
 *   the kernel heap, and retain the completed cwd after a later failed chdir
 */

#include "../../../root/crt/sys.h"
#include "../../../root/crt/string.h"
#include "../../user_test.h"

#define DEEP_DIRECTORY_PATH "/d00/d01/d02/d03/d04/d05/d06/d07/d08/d09/d10/d11/d12/d13/d14/d15/d16/d17/d18/d19"
#define DEEP_FILE_PATH "/d00/d01/d02/d03/d04/d05/d06/d07/d08/d09/d10/d11/d12/d13/d14/d15/d16/d17/d18/d19/marker"

int main(void){ /* Exercise the user file-system syscall suite. */
  char buf[8];
  char deep_cwd[128];
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

  // `open` creates this missing hierarchy. The subsequent chdir forces cwd
  // canonicalization to retain 20 live components, directly exceeding the
  // old fixed 16-pointer scratch array.
  fd = open(DEEP_FILE_PATH);
  user_test_expect_eq("create deep marker", fd >= 0, 1);
  user_test_expect_eq("close deep marker", close(fd), 0);
  user_test_expect_eq("chdir deep path", chdir(DEEP_DIRECTORY_PATH), 0);
  user_test_expect_eq("getcwd deep path",
    (int)getcwd(deep_cwd, sizeof(deep_cwd)) != -1, 1);
  user_test_expect_eq("deep cwd canonical path",
    strcmp(deep_cwd, DEEP_DIRECTORY_PATH), 0);
  user_test_expect_eq("failed chdir from deep cwd",
    chdir("definitely_missing"), -1);
  user_test_expect_eq("getcwd retained after failed chdir",
    (int)getcwd(deep_cwd, sizeof(deep_cwd)) != -1, 1);
  user_test_expect_eq("failed chdir kept cwd pair",
    strcmp(deep_cwd, DEEP_DIRECTORY_PATH), 0);

  yield();
  user_test_expect_eq("yield resumed the current process", 1, 1);

  return 0;
}
