/*
 * user_fs_mutation_syscalls guest:
 * - validate the user-visible mkdir(), rmdir(), and unlink() wrappers
 * - ensure mkdir() creates one traversable empty directory in the current cwd
 * - ensure unlink() removes only non-directory entries
 * - ensure rmdir() removes only empty directories and rejects file targets
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
 */

#include "../../../root/crt/print.h"
#include "../../../root/crt/sys.h"

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

  puts("***mkdir bad path pointer\n");
  test_syscall(mkdir(BAD_LOW_PTR));

  puts("***rmdir bad path pointer\n");
  test_syscall(rmdir(BAD_LOW_PTR));

  puts("***unlink bad path pointer\n");
  test_syscall(unlink(BAD_LOW_PTR));

  puts("***slash path rejection\n");
  test_syscall(mkdir("a/b"));
  test_syscall(rmdir("a/b"));
  test_syscall(unlink("a/b"));

  puts("***mkdir create and traverse\n");
  test_syscall(mkdir(MADE_DIR));
  test_syscall(chdir(MADE_DIR));
  test_syscall(chdir(".."));

  puts("***mkdir duplicate\n");
  test_syscall(mkdir(MADE_DIR));

  puts("***unlink regular file\n");
  test_syscall(write_one_byte_file(VICTIM_FILE, VICTIM_BYTE));
  test_syscall(read_one_byte_file(VICTIM_FILE, &byte));
  test_syscall(byte);
  test_syscall(unlink(VICTIM_FILE));
  test_syscall(read_one_byte_file(VICTIM_FILE, &byte));
  test_syscall(unlink(VICTIM_FILE));

  puts("***unlink rejects directory\n");
  test_syscall(mkdir(UNLINK_DIR));
  test_syscall(unlink(UNLINK_DIR));
  test_syscall(chdir(UNLINK_DIR));
  test_syscall(chdir(".."));
  test_syscall(rmdir(UNLINK_DIR));

  puts("***rmdir rejects file\n");
  test_syscall(write_one_byte_file(FILE_TARGET, FILE_TARGET_BYTE));
  test_syscall(rmdir(FILE_TARGET));
  test_syscall(unlink(FILE_TARGET));

  puts("***rmdir rejects non-empty directory\n");
  test_syscall(mkdir(NONEMPTY_DIR));
  test_syscall(chdir(NONEMPTY_DIR));
  test_syscall(write_one_byte_file(NESTED_FILE, NESTED_BYTE));
  test_syscall(chdir(".."));
  test_syscall(rmdir(NONEMPTY_DIR));
  test_syscall(chdir(NONEMPTY_DIR));
  test_syscall(unlink(NESTED_FILE));
  test_syscall(chdir(".."));
  test_syscall(rmdir(NONEMPTY_DIR));

  puts("***rmdir empty directory\n");
  test_syscall(rmdir(MADE_DIR));
  test_syscall(chdir(MADE_DIR));
  test_syscall(mkdir(MADE_DIR));
  test_syscall(rmdir(MADE_DIR));

  return 0;
}
