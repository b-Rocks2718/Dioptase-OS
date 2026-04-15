#include "../../../crt/constants.h"
#include "../../../crt/sys.h"
#include "../../../crt/print.h"

int main(void){
  chdir("./files");
  int fd = open("hello.txt");
  char buffer[128];
  int n = read(fd, buffer, sizeof(buffer) - 1);
  if (n > 0){
    buffer[n] = '\0';
    puts(buffer);
  }
  close(fd);

  
}
