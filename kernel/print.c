#include "print.h"
#include "machine.h"
#include "atomic.h"

int print_lock = 0;

unsigned puts(char* str){
  unsigned count = 0;
  while (*str != '\0'){
    putchar(*str);
    ++str;
    ++count;
  }
  return count;
}

unsigned say(char* fmt, int* arr){
  spin_lock_get(&print_lock);
  unsigned count = printf(fmt, arr);
  spin_lock_release(&print_lock);
  return count;
}

// simple printf implementation supporting %d, %u, %x, %X
// accepts an array instead of variadic arguments
unsigned printf(char* fmt, int* arr){
  unsigned count = 0;
  unsigned i = 0;
  while (*fmt != '\0'){
    if (*fmt == '%') {
      if (*(fmt + 1) == 'd') {
        ++fmt;
        count += print_signed(arr[i++]);
      } else if (*(fmt + 1) == 'u') {
        ++fmt;
        count += print_unsigned((unsigned)arr[i++]);
      } else if (*(fmt + 1) == 'x') {
        ++fmt;
        count += print_hex((unsigned)arr[i++], false);
      } else if (*(fmt + 1) == 'X') {
        ++fmt;
        count += print_hex((unsigned)arr[i++], true);
      } else {
        // unsupported format specifier, print as is
        putchar(*fmt);
        ++count;
      }
    } else {
      putchar(*fmt);
      ++count;
    }
    ++fmt;
  }
  return count;
}

unsigned print_signed(int n){
  if(n == 0){
      putchar('0');
      return 1;
  }
  if(n < 0){
      putchar('-');
      n = -n;
  }

  unsigned d = n % 10;
  n = n / 10;
  
  unsigned count = 1;

  if (n != 0){
    count += print_signed(n);
  } 
  putchar('0' + d);
  
  // return number of characters printed
  return count;
}

unsigned print_unsigned(unsigned n){
  if(n == 0){
      putchar('0');
      return 1;
  }

  unsigned d = n % 10;
  n = n / 10;
  
  unsigned count = 1;

  if (n != 0){
    count += print_unsigned(n);
  } 
  putchar('0' + d);
  
  // return number of characters printed
  return count;
}

unsigned print_hex(unsigned n, bool uppercase){
  if(n == 0){
      putchar('0');
      return 1;
  }

  unsigned d = n % 16;
  n = n / 16;
  
  unsigned count = 1;

  if (n != 0){
    count += print_hex(n, uppercase);
  } 
  if (d < 10){
    putchar('0' + d);
  } else {
    putchar((uppercase ? 'A' : 'a') + (d - 10));
  }
  
  // return number of characters printed
  return count;
}
