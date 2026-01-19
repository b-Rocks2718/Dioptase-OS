#include "kernel/machine.h"
#include "kernel/print.h"

unsigned next_collatz(unsigned x){
  if (x & 1){
    // if x is odd, return 3 * x + 1
    return x + x + x + 1;
  } else {
    // if x is even, return x/2
    return x >> 1;
  }
}

unsigned print_collatz_seq(unsigned x){
  unsigned max = x;
  unsigned i = 0;

  puts("Collatz sequence:\n");
  
  while (x != 1){
    print_num(x);
    putchar(',');
    x = next_collatz(x);
    if (x > max) max = x;
    ++i;
  }
  print_num(x);
  puts("\n\nMax: ");
  print_num(max);
  putchar('\n');
}

int kernel_entry(void) {
  if (get_core_id() == 0){
    print_collatz_seq(67);
  } else {
    while (1);
  }
  return 67;
}
