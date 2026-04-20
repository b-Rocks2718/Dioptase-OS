/*
 * Tests the following system calls:
 * - getdents.
 * - getcwd.
 * - readlink.
 */

#include "../../../crt/sys.h"
#include "../../../crt/print.h"
#include "dirs.h"

// int getdents(int fd, char* buffer, unsigned buffer_size);

// int getcwd(char* buffer, unsigned buffer_size);

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
  int n = getcwd(buffer, 100);
  int args[1] = {(int) buffer};
  printf("***%s\n", args);

  chdir("./folder/inner_folder0");
  n = getcwd(buffer, 100);
  printf("***%s\n", args);

  chdir("../inner_folder1/../inner_folder1/./");
  n = getcwd(buffer, 100);
  printf("***%s\n", args);

  chdir("/folder"); // Absolute path.
  n = getcwd(buffer, 100);
  printf("***%s\n", args);

  chdir("../././");
  n = getcwd(buffer, 100);
  printf("***%s\n", args);

  // readlink.
  n = readlink("folder/symlink_file", buffer, 100);
  printf("***%s\n", args);

  n = readlink("folder/symlink_folder", buffer, 100);
  printf("***%s\n", args);

  // getdents.
  struct LinkedDirent* entries = read_directory("/folder");
  destroy_linked_dirents(entries);
  
  printf("***Done.\n", NULL);

  return 0;
}
