/*
 * Exec source image:
 * - forks so the parent can observe the replacement image's exit status
 * - passes two arguments through execv for copied-string and NULL-sentinel
 *   validation in /test/init
 */

#include "../../../root/crt/print.h"
#include "../../../root/crt/sys.h"

int main(void) { /* Verify launching the minimal user executable. */
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
