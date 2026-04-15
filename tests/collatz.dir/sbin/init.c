#include "../../../crt/print.h"
#include "../../../crt/sys.h"
#include "../../../crt/constants.h"

// Compute the next Collatz value for one positive integer.
unsigned next_collatz(unsigned x){
  return (x & 1) ? (3 * x + 1) : (x / 2);
}

// Build the full Collatz sequence from x down to 1.
void print_collatz_seq(unsigned x){
  puts("***");
  while (x != 1){
    // Append each intermediate value before advancing to the next one.
    printf("%u,", &x);

    x = next_collatz(x);
  }

  print_unsigned(x);
  putchar('\n');
}

// Read one start value, print its Collatz sequence, and free the list.
int main(void) {
  puts("***Collatz sequence:\n");
  print_collatz_seq(67);
  puts("***Collatz sequence done\n");

  return 67;
}
