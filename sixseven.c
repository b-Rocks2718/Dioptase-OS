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

  putchar(67); // C
  putchar(111); // o
  putchar(108); // l
  putchar(108); // l
  putchar(97); // a
  putchar(116); // t
  putchar(122); // z
  putchar(58); // :
  putchar(10); // \n
  putchar(10);
  

  while (x != 1){
    print_num(x);
    putchar(44); // ,
    x = next_collatz(x);
    if (x > max) max = x;
    ++i;
  }
  print_num(x);
  putchar(10); //\n
  putchar(10); //\n
  putchar(77); // M
  putchar(97); // a
  putchar(120); // x
  putchar(58); // :
  putchar(32); // space
  print_num(max);
  putchar(10);
}

int kernel_entry(void) {
  print_collatz_seq(67);
  return 67;
}
