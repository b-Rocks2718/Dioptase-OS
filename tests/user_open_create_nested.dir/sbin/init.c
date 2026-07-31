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
#include "../../user_test.h"

#define CREATED_FILE_PATH "created/by/open/note.txt"
#define CREATED_DIR_PATH "created/by/open"
#define CREATED_FILE_BASENAME "note.txt"
#define CREATED_FILE_BYTE 'N'

int main(void){
  char buf[1];
  char out = CREATED_FILE_BYTE;

  int fd = open(CREATED_FILE_PATH);
  user_test_expect_eq("create file with missing parent directories",
    fd >= 0, 1);
  user_test_expect_eq("write(fd, &out, 1)", write(fd, &out, 1), 1);
  user_test_expect_eq("seek(fd, 0, SEEK_SET)", seek(fd, 0, SEEK_SET), 0);
  user_test_expect_eq("read(fd, buf, 1)", read(fd, buf, 1), 1);
  user_test_expect_eq("new nested file byte", buf[0], CREATED_FILE_BYTE);
  user_test_expect_eq("close(fd)", close(fd), 0);

  user_test_expect_eq("chdir(CREATED_DIR_PATH)", chdir(CREATED_DIR_PATH), 0);
  fd = open(CREATED_FILE_BASENAME);
  user_test_expect_eq("reopen created file by basename", fd >= 0, 1);
  user_test_expect_eq("read(fd, buf, 1)", read(fd, buf, 1), 1);
  user_test_expect_eq("reopened nested file byte", buf[0], CREATED_FILE_BYTE);
  user_test_expect_eq("close(fd)", close(fd), 0);

  return 0;
}
