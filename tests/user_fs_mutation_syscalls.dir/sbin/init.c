/*
 * user_fs_mutation_syscalls guest:
 * - validate the user-visible mkdir(), rmdir(), and unlink() wrappers
 * - ensure mkdir() creates one traversable empty directory in the current cwd
 * - ensure unlink() removes only non-directory entries
 * - ensure rmdir() removes only empty directories and rejects file targets
 * - accept a 255-byte mkdir basename and reject a 256-byte basename
 *
 * How:
 * - reject one invalid user pointer and one slash-separated name for each new
 *   syscall
 * - create a directory, chdir into it and back out, reject a duplicate create,
 *   remove it with rmdir(), and recreate it
 * - create and unlink a regular file, then reopen the same pathname to confirm
 *   open() recreated it as an empty file
 * - verify unlink() rejects directories, rmdir() rejects a file, and rmdir()
 *   rejects a non-empty directory until its nested file is removed
 * - construct exact-limit and overlong mkdir names in guest memory
 */

#include "../../../root/crt/print.h"
#include "../../../root/crt/sys.h"
#include "../../user_test.h"

#define BAD_LOW_PTR ((char*)0x1000)
#define MADE_DIR "made"
#define UNLINK_DIR "unlink-dir"
#define NONEMPTY_DIR "nonempty"
#define NESTED_FILE "note.txt"
#define VICTIM_FILE "victim.txt"
#define FILE_TARGET "file-target.txt"
#define VICTIM_BYTE 'V'
#define FILE_TARGET_BYTE 'F'
#define NESTED_BYTE 'N'
#define EXT2_BASENAME_MAX_BYTES 255
#define EXT2_BASENAME_BUFFER_BYTES 256
#define EXT2_OVERLONG_BASENAME_BYTES 256
#define EXT2_OVERLONG_BASENAME_BUFFER_BYTES 257

// Fill one basename with ordinary bytes that cannot be mistaken for a path
// separator or either reserved dot entry.
static void fill_basename(char* dest, unsigned size){
  for (unsigned i = 0; i < size; ++i){
    dest[i] = 'm';
  }
  dest[size] = '\0';
}

static int write_one_byte_file(char* path, char value){
  int fd = open(path);
  int rc;

  if (fd < 0){
    return -1;
  }

  rc = write(fd, &value, 1);
  if (rc != 1){
    close(fd);
    return -1;
  }

  rc = close(fd);
  if (rc != 0){
    return -1;
  }

  return 0;
}

static int read_one_byte_file(char* path, char* out){
  int fd = open(path);
  int rc;

  if (fd < 0){
    return -1;
  }

  rc = read(fd, out, 1);
  if (close(fd) != 0){
    return -1;
  }

  return rc;
}

int main(void){
  char byte = 0;
  char exact_name[EXT2_BASENAME_BUFFER_BYTES];
  char overlong_name[EXT2_OVERLONG_BASENAME_BUFFER_BYTES];

  puts("***mkdir bad path pointer\n");
  user_test_expect_eq("mkdir(BAD_LOW_PTR)", mkdir(BAD_LOW_PTR), -1);

  puts("***rmdir bad path pointer\n");
  user_test_expect_eq("rmdir(BAD_LOW_PTR)", rmdir(BAD_LOW_PTR), -1);

  puts("***unlink bad path pointer\n");
  user_test_expect_eq("unlink(BAD_LOW_PTR)", unlink(BAD_LOW_PTR), -1);

  puts("***slash path rejection\n");
  user_test_expect_eq("mkdir(\"a/b\")", mkdir("a/b"), -1);
  user_test_expect_eq("rmdir(\"a/b\")", rmdir("a/b"), -1);
  user_test_expect_eq("unlink(\"a/b\")", unlink("a/b"), -1);

  puts("***mkdir basename length boundary\n");
  fill_basename(exact_name, EXT2_BASENAME_MAX_BYTES);
  fill_basename(overlong_name, EXT2_OVERLONG_BASENAME_BYTES);
  user_test_expect_eq("mkdir accepts 255-byte basename",
    mkdir(exact_name), 0);
  user_test_expect_eq("rmdir removes 255-byte basename",
    rmdir(exact_name), 0);
  user_test_expect_eq("mkdir rejects 256-byte basename",
    mkdir(overlong_name), -1);

  puts("***mkdir create and traverse\n");
  user_test_expect_eq("mkdir(MADE_DIR)", mkdir(MADE_DIR), 0);
  user_test_expect_eq("chdir(MADE_DIR)", chdir(MADE_DIR), 0);
  user_test_expect_eq("chdir(\"..\")", chdir(".."), 0);

  puts("***mkdir duplicate\n");
  user_test_expect_eq("mkdir(MADE_DIR)", mkdir(MADE_DIR), -1);

  puts("***unlink regular file\n");
  user_test_expect_eq("write_one_byte_file(VICTIM_FILE, VICTIM_BYTE)", write_one_byte_file(VICTIM_FILE, VICTIM_BYTE), 0);
  user_test_expect_eq("read_one_byte_file(VICTIM_FILE, &byte)", read_one_byte_file(VICTIM_FILE, &byte), 1);
  user_test_expect_eq("victim file byte before unlink", byte, VICTIM_BYTE);
  user_test_expect_eq("unlink(VICTIM_FILE)", unlink(VICTIM_FILE), 0);
  user_test_expect_eq("read_one_byte_file(VICTIM_FILE, &byte)", read_one_byte_file(VICTIM_FILE, &byte), 0);
  user_test_expect_eq("unlink(VICTIM_FILE)", unlink(VICTIM_FILE), 0);

  puts("***unlink rejects directory\n");
  user_test_expect_eq("mkdir(UNLINK_DIR)", mkdir(UNLINK_DIR), 0);
  user_test_expect_eq("unlink(UNLINK_DIR)", unlink(UNLINK_DIR), -1);
  user_test_expect_eq("chdir(UNLINK_DIR)", chdir(UNLINK_DIR), 0);
  user_test_expect_eq("chdir(\"..\")", chdir(".."), 0);
  user_test_expect_eq("rmdir(UNLINK_DIR)", rmdir(UNLINK_DIR), 0);

  puts("***rmdir rejects file\n");
  user_test_expect_eq("write_one_byte_file(FILE_TARGET, FILE_TARGET_BYTE)", write_one_byte_file(FILE_TARGET, FILE_TARGET_BYTE), 0);
  user_test_expect_eq("rmdir(FILE_TARGET)", rmdir(FILE_TARGET), -1);
  user_test_expect_eq("unlink(FILE_TARGET)", unlink(FILE_TARGET), 0);

  puts("***rmdir rejects non-empty directory\n");
  user_test_expect_eq("mkdir(NONEMPTY_DIR)", mkdir(NONEMPTY_DIR), 0);
  user_test_expect_eq("chdir(NONEMPTY_DIR)", chdir(NONEMPTY_DIR), 0);
  user_test_expect_eq("write_one_byte_file(NESTED_FILE, NESTED_BYTE)", write_one_byte_file(NESTED_FILE, NESTED_BYTE), 0);
  user_test_expect_eq("chdir(\"..\")", chdir(".."), 0);
  user_test_expect_eq("rmdir(NONEMPTY_DIR)", rmdir(NONEMPTY_DIR), -1);
  user_test_expect_eq("chdir(NONEMPTY_DIR)", chdir(NONEMPTY_DIR), 0);
  user_test_expect_eq("unlink(NESTED_FILE)", unlink(NESTED_FILE), 0);
  user_test_expect_eq("chdir(\"..\")", chdir(".."), 0);
  user_test_expect_eq("rmdir(NONEMPTY_DIR)", rmdir(NONEMPTY_DIR), 0);

  puts("***rmdir empty directory\n");
  user_test_expect_eq("rmdir(MADE_DIR)", rmdir(MADE_DIR), 0);
  user_test_expect_eq("chdir(MADE_DIR)", chdir(MADE_DIR), -1);
  user_test_expect_eq("mkdir(MADE_DIR)", mkdir(MADE_DIR), 0);
  user_test_expect_eq("rmdir(MADE_DIR)", rmdir(MADE_DIR), 0);

  return 0;
}
