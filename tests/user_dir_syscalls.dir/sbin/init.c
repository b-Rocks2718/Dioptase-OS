/*
 * Tests the following system calls:
 * - getdents.
 * - getcwd.
 * - readlink.
 */

#include "../../../crt/sys.h"

unsigned strlen(char* str) {
  unsigned len = 0;
  while (str[len] != '\0') {
    len++;
  }
  return len;
}

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

void print_buffer(char* buffer, unsigned size) {
  char* asterisks = "***";
  if (write_all(asterisks, 3) != 0) {
    return;
  }
  if (write_all(buffer, size) != 0) {
    return;
  }
  char* newline = "\n";
  if (write_all(newline, 1) != 0) {
    return;
  }
}

int main(void){
  char buffer[100];
  int n = getcwd(buffer, 100);
  print_buffer(buffer, strlen(buffer));

  chdir("./folder/inner_folder0");
  n = getcwd(buffer, 100);
  print_buffer(buffer, strlen(buffer));

  chdir("../inner_folder1/../inner_folder1/./");
  n = getcwd(buffer, 100);
  print_buffer(buffer, strlen(buffer));

  chdir("/folder"); // Absolute path.
  n = getcwd(buffer, 100);
  print_buffer(buffer, strlen(buffer));

  chdir("../././");
  n = getcwd(buffer, 100);
  print_buffer(buffer, strlen(buffer));

  return 0;
}
