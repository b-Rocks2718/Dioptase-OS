/*
 * Fork descriptor/lifecycle regression:
 * - the parent receives one descriptor naming the new child
 * - that descriptor is not inherited back into the child itself
 * - the child exits normally and the parent consumes its result once
 */

#include "../../../root/crt/print.h"
#include "../../../root/crt/sys.h"

#define FIRST_CHILD_DESCRIPTOR 200

int main(void) { /* Verify fork creates a child with independent return values. */
  puts("***hello from fork test\n");

  int child = fork();
  if (child == 0){
    int self_signal = signal_child(FIRST_CHILD_DESCRIPTOR, SIGNAL_KILL);
    puts("***child self descriptor result: ");
    print_signed(self_signal);
    puts("\n");
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
