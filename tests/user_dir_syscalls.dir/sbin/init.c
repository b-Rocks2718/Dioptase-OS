/*
 * Tests the following system calls:
 * - getdents.
 * - getcwd.
 * - readlink.
 * - create a missing leaf through an intermediate directory symlink.
 * - reject zero-size, interior, and beyond-EOF directory reads, while a
 *   too-small buffer leaves the next live entry available for a retry
 */

#include "../../../root/crt/sys.h"
#include "../../../root/crt/print.h"
#include "../../../root/crt/stdlib.h"
#include "../../user_test.h"
#include "dirs.h"

// int getdents(int fd, char* buffer, unsigned buffer_size);

// char* getcwd(char* buffer, unsigned buffer_size);

// int readlink(char* path, char* buffer, unsigned buffer_size);

int write_all(char* buffer, unsigned size) {
  unsigned written = 0;
  while (written < size) {
    int new_written = write(STDOUT, buffer + written, size - written);
    if (new_written < 0) {
      return -1;
    }
    written += new_written;
  }
  return 0;
}

int main(void){
  // getcwd.
  char buffer[100];
  getcwd(buffer, 100);
  int args[1] = {(int) buffer};
  printf("***%s\n", args);

  chdir("./folder/inner_folder0");
  getcwd(buffer, 100);
  printf("***%s\n", args);

  chdir("../inner_folder1/../inner_folder1/./");
  getcwd(buffer, 100);
  printf("***%s\n", args);

  chdir("/folder"); // Absolute path.
  getcwd(buffer, 100);
  printf("***%s\n", args);

  chdir("../././");
  getcwd(buffer, 100);
  printf("***%s\n", args);

  // readlink.
  int n = readlink("folder/symlink_file", buffer, 100);
  printf("***%s\n", args);

  n = readlink("folder/symlink_folder", buffer, 100);
  printf("***%s\n", args);

  // The creating open path first fails whole-path lookup, then walks existing
  // components one at a time. Each component lookup must retain node_find's
  // symlink expansion semantics rather than treating the link inode as a
  // non-directory or creating beside it.
  int symlink_create_fd =
    open("/folder/symlink_folder/created-through-link");
  user_test_expect_eq("create leaf through intermediate directory symlink",
    symlink_create_fd >= 0, 1);
  user_test_expect_eq("close leaf created through symlink",
    symlink_create_fd >= 0 ? close(symlink_create_fd) : -1, 0);
  symlink_create_fd =
    open_existing("/folder/inner_folder0/created-through-link");
  user_test_expect_eq("symlink create published in target directory",
    symlink_create_fd >= 0, 1);
  user_test_expect_eq("close direct target lookup",
    symlink_create_fd >= 0 ? close(symlink_create_fd) : -1, 0);

  // The first two ext2 records are `.` and `..`; each converts to a 16-byte
  // linux_dirent. Fifteen bytes must emit nothing and retain offset zero. Two
  // subsequent 16-byte calls must therefore return those entries in order.
  char small_dirent_buffer[16];
  int edge_fd = open("/folder");
  user_test_expect_eq("open directory for getdents edges", edge_fd >= 0, 1);
  user_test_expect_eq("getdents rejects zero buffer",
    getdents(edge_fd, small_dirent_buffer, 0), -1);
  user_test_expect_eq("seek directory to interior byte",
    seek(edge_fd, 1, SEEK_SET), 1);
  user_test_expect_eq("getdents rejects interior offset",
    getdents(edge_fd, small_dirent_buffer, sizeof(small_dirent_buffer)), -1);
  user_test_expect_eq("reset directory offset", seek(edge_fd, 0, SEEK_SET), 0);
  user_test_expect_eq("getdents preserves entry that does not fit",
    getdents(edge_fd, small_dirent_buffer, 15), 0);
  user_test_expect_eq("too-small getdents retains offset",
    seek(edge_fd, 0, SEEK_CUR), 0);
  user_test_expect_eq("getdents emits dot after retry",
    getdents(edge_fd, small_dirent_buffer, sizeof(small_dirent_buffer)), 16);
  struct linux_dirent* small_entry =
    (struct linux_dirent*)small_dirent_buffer;
  user_test_expect_eq("first retried entry is dot",
    streq((char*)&small_entry->d_name, "."), 1);
  user_test_expect_eq("getdents does not skip dot-dot",
    getdents(edge_fd, small_dirent_buffer, sizeof(small_dirent_buffer)), 16);
  user_test_expect_eq("second retried entry is dot-dot",
    streq((char*)&small_entry->d_name, ".."), 1);
  user_test_expect_eq("seek one byte beyond directory EOF",
    seek(edge_fd, 1, SEEK_END) > 0, 1);
  user_test_expect_eq("getdents rejects beyond-EOF offset",
    getdents(edge_fd, small_dirent_buffer, sizeof(small_dirent_buffer)), -1);
  user_test_expect_eq("seek to exact directory EOF",
    seek(edge_fd, 0, SEEK_END) > 0, 1);
  user_test_expect_eq("getdents accepts exact EOF",
    getdents(edge_fd, small_dirent_buffer, sizeof(small_dirent_buffer)), 0);
  user_test_expect_eq("close getdents edge descriptor", close(edge_fd), 0);

  // getdents.
  struct LinkedDirent* entries = read_directory("/folder");
  destroy_linked_dirents(entries);

  int fd = open("/folder");
  printf("***Starting getdents fork test with shared file descriptor.\n", NULL);

  int pid = fork();
  if (pid == 0) {
    // Child.
    char* child_buffer = (char*) malloc(1024);
    int child_getdents_n = getdents(fd, child_buffer, 1024);
    free(child_buffer);
    return child_getdents_n;
  } else {
    // Parent.
    char* parent_buffer = (char*) malloc(1024);
    int parent_getdents_n = getdents(fd, parent_buffer, 1024);
    free(parent_buffer);
    int rc = wait_child(pid);
    int getdents_print_args[1] = {rc + parent_getdents_n};
    printf("***Total getdents bytes: %d\n", getdents_print_args);
  }

  // unlink's documented non-directory set includes symlinks. Perform this
  // after all directory-list expectations so removing the fixture cannot
  // perturb the getdents totals above.
  user_test_expect_eq("chdir folder for symlink unlink", chdir("/folder"), 0);
  user_test_expect_eq("unlink accepts symlink", unlink("symlink_file"), 0);
  user_test_expect_eq("unlinked symlink is gone",
    readlink("symlink_file", buffer, sizeof(buffer)), -1);
  
  printf("***Done.\n", NULL);

  return 0;
}
