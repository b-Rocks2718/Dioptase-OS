#include "../../../crt/print.h"
#include "../../../crt/sys.h"
#include "../../../crt/constants.h"

int main(void) {
  puts("***hello from fork test\n");

  if (fork() == 0){
    puts("***hello from child\n");
  } else {
    puts("***hello from parent\n");
  }

  return 67;
}
