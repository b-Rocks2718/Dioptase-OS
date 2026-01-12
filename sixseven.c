#include "kernel/machine.h"
#include "kernel/collatz.h"

int kernel_entry(void) {
  print_collatz_seq(67);
  return 67;
}
