#include "../../../crt/print.h"
#include "../../../crt/sys.h"
#include "../../../crt/constants.h"

int main(void) {
  puts("***hello from fork test\n");

  int child = fork();
  if (child == 0){
    puts("***hello from child\n");

    return 42;
  } else {
    int rc = wait_child(child);
    puts("***hello from parent\n");
    puts("***child returned with status: ");
    print_unsigned(rc);
    puts("\n");
  }

  return 67;
}
