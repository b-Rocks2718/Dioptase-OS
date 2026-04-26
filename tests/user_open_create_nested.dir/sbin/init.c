/*
 * user_open_create_nested guest:
 * - verify open() creates missing intermediate directories and the final file
 *   when given a multi-component relative path
 * - verify the created file can be reopened through the newly created
 *   directory path after closing the original descriptor
 *
 * How:
 * - open a path whose parent directories do not exist yet, then write and read
 *   back one byte through the original descriptor
 * - chdir into the directory tree that open() created, reopen the file by its
 *   basename, and confirm the previously written byte persisted
 */

#include "../../../root/crt/sys.h"

#define CREATED_FILE_PATH "created/by/open/note.txt"
#define CREATED_DIR_PATH "created/by/open"
#define CREATED_FILE_BASENAME "note.txt"
#define CREATED_FILE_BYTE 'N'

int main(void){
  char buf[1];
  char out = CREATED_FILE_BYTE;

  int fd = open(CREATED_FILE_PATH);
  test_syscall(fd >= 0);
  test_syscall(write(fd, &out, 1));
  test_syscall(seek(fd, 0, SEEK_SET));
  test_syscall(read(fd, buf, 1));
  test_syscall(buf[0]);
  test_syscall(close(fd));

  test_syscall(chdir(CREATED_DIR_PATH));
  fd = open(CREATED_FILE_BASENAME);
  test_syscall(fd >= 0);
  test_syscall(read(fd, buf, 1));
  test_syscall(buf[0]);
  test_syscall(close(fd));

  return 0;
}
