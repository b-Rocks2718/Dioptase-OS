#include "print.h"
#include "machine.h"

int print_lock = 0;

static int ZERO_CHAR = 48;
static int MINUS_CHAR = 45;

int print_num(int n){
  if(n == 0){
      putchar(ZERO_CHAR);
      return 1;
  }
  if(n < 0){
      putchar(MINUS_CHAR);
      n = -n;
  }

  int d = n % 10;
  n = n / 10;
  
  int count = 1;

  if (n != 0){
    count += print_num(n);
  } 
  putchar(ZERO_CHAR + d);
  
  // return number of characters printed
  return count;
}