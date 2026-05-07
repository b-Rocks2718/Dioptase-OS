/*
 * Tests the following system calls:
 * - getdents.
 * - getcwd.
 * - readlink.
 */

#include "../../../root/crt/sys.h"
#include "../../../root/crt/print.h"
#include "../../../root/crt/stdlib.h"
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
  
  printf("***Done.\n", NULL);

  return 0;
}
