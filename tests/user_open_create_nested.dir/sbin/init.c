/*
 * user_open_create_nested guest:
 * - verify open() creates missing intermediate directories and the final file
 *   when given a multi-component relative path
 * - verify the created file can be reopened through the newly created
 *   directory path after closing the original descriptor
 * - accept an exact 255-byte basename and reject a 256-byte component before
 *   creating any earlier missing parent directory
 *
 * How:
 * - open a path whose parent directories do not exist yet, then write and read
 *   back one byte through the original descriptor
 * - chdir into the directory tree that open() created, reopen the file by its
 *   basename, and confirm the previously written byte persisted
 * - construct boundary-length names in guest memory and verify an overlong
 *   later component leaves its proposed parent absent
 */

#include "../../../root/crt/sys.h"
#include "../../../root/crt/string.h"
#include "../../user_test.h"

#define CREATED_FILE_PATH "created/by/open/note.txt"
#define CREATED_DIR_PATH "created/by/open"
#define CREATED_FILE_BASENAME "note.txt"
#define CREATED_FILE_BYTE 'N'
#define EXT2_BASENAME_MAX_BYTES 255
#define EXT2_BASENAME_BUFFER_BYTES 256
#define EXT2_OVERLONG_BASENAME_BYTES 256
#define EXT2_OVERLONG_BASENAME_BUFFER_BYTES 257
#define OVERLONG_PARENT "overlong-parent"
#define OVERLONG_PARENT_BYTES 15
#define OVERLONG_PATH_BUFFER_BYTES 273

// Fill one user pathname component with ordinary non-separator bytes.
static void fill_basename(char* dest, unsigned size){
  for (unsigned i = 0; i < size; ++i){
    dest[i] = 'u';
  }
  dest[size] = '\0';
}

// Build "overlong-parent/<name>" without relying on formatted output.
static void make_overlong_nested_path(char* dest, char* name){
  memcpy(dest, OVERLONG_PARENT, OVERLONG_PARENT_BYTES);
  dest[OVERLONG_PARENT_BYTES] = '/';
  memcpy(dest + OVERLONG_PARENT_BYTES + 1, name,
    EXT2_OVERLONG_BASENAME_BYTES + 1);
}

int main(void){
  char buf[1];
  char out = CREATED_FILE_BYTE;
  char exact_name[EXT2_BASENAME_BUFFER_BYTES];
  char overlong_name[EXT2_OVERLONG_BASENAME_BUFFER_BYTES];
  char overlong_path[OVERLONG_PATH_BUFFER_BYTES];

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

  fill_basename(exact_name, EXT2_BASENAME_MAX_BYTES);
  fd = open(exact_name);
  user_test_expect_eq("open accepts 255-byte basename", fd >= 0, 1);
  if (fd >= 0){
    user_test_expect_eq("close 255-byte basename", close(fd), 0);
  }
  user_test_expect_eq("unlink 255-byte basename", unlink(exact_name), 0);

  fill_basename(overlong_name, EXT2_OVERLONG_BASENAME_BYTES);
  user_test_expect_eq("open rejects 256-byte basename",
    open(overlong_name), -1);

  make_overlong_nested_path(overlong_path, overlong_name);
  user_test_expect_eq("open rejects nested 256-byte component",
    open(overlong_path), -1);
  user_test_expect_eq("overlong nested open creates no parent",
    chdir(OVERLONG_PARENT), -1);

  return 0;
}
