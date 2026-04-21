#include "../../../crt/print.h"
#include "../../../crt/sys.h"
#include "../../../crt/constants.h"

int main(void) {
  puts("***hello from exec test\n");

  int child = fork();
  if (child < 0){
    puts("***fork failed\n");
    return -1;
  } else if (child == 0){
    char* argv[2] = {"/test/init", "hello!"};
    int rc = execv("/test/init", 2, argv);
    puts("***execv returned ");
    print_signed(rc);
    puts("\n");
  } else {
    int rc = wait_child(child);
    puts("***wait_child returned ");
    print_signed(rc);
    puts("\n");
  }

  return 67;
}
