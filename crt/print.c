#include "print.h"
#include "sys.h"

#define DECIMAL_BASE 10u
#define HEX_BASE 16u
#define MAX_INT_DEC_DIGITS 10 
#define MAX_UNSIGNED_HEX_DIGITS 8 

void putchar(char c){
  while (!write(STDOUT, &c, 1));
}

unsigned puts(char* str){
  unsigned count = 0;
  while(*str){
    putchar(*str++);
    ++count;
  }
  return count;
}

unsigned printf(char* fmt, void* arr){
  unsigned count = 0;
  unsigned i = 0;
  while (*fmt != '\0'){
    if (*fmt == '%') {
      if (*(fmt + 1) == 'd') {
        ++fmt;
        count += print_signed(((int*)arr)[i++]);
      } else if (*(fmt + 1) == 'u') {
        ++fmt;
        count += print_unsigned(((unsigned*)arr)[i++]);
      } else if (*(fmt + 1) == 'x') {
        ++fmt;
        count += print_hex(((unsigned*)arr)[i++], false);
      } else if (*(fmt + 1) == 'X') {
        ++fmt;
        count += print_hex(((unsigned*)arr)[i++], true);
      } else if (*(fmt + 1) == 's') {
        ++fmt;
        count += puts((char*)((void**)arr)[i++]);
      } else if (*(fmt + 1) == 'c') {
        ++fmt;
        putchar(((unsigned*)arr)[i++]);
        ++count;
      } else if (*(fmt + 1) == '%') {
        ++fmt;
        putchar('%');
        ++count;
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
  char digits[MAX_INT_DEC_DIGITS];
  unsigned magnitude;
  unsigned len = 0;
  unsigned count;

  if(n == 0){
    putchar('0');
    return 1;
  }

  if(n < 0){
    putchar('-');
    magnitude = 0u - (unsigned)n;
  } else {
    magnitude = (unsigned)n;
  }

  while (magnitude != 0){
    digits[len++] = (char)('0' + (magnitude % DECIMAL_BASE));
    magnitude /= DECIMAL_BASE;
  }

  count = len;
  while (len != 0){
    putchar(digits[--len]);
  }
  
  // return number of characters printed
  return (n < 0) ? (count + 1) : count;
}

unsigned print_unsigned(unsigned n){
  char digits[MAX_INT_DEC_DIGITS];
  unsigned len = 0;
  unsigned count;

  if(n == 0){
    putchar('0');
    return 1;
  }

  while (n != 0){
    digits[len++] = (char)('0' + (n % DECIMAL_BASE));
    n /= DECIMAL_BASE;
  }

  count = len;
  while (len != 0){
    putchar(digits[--len]);
  }
  
  // return number of characters printed
  return count;
}

unsigned print_hex(unsigned n, bool uppercase){
  char digits[MAX_UNSIGNED_HEX_DIGITS];
  unsigned len = 0;
  unsigned count;

  if(n == 0){
    putchar('0');
    return 1;
  }

  while (n != 0){
    unsigned digit = n % HEX_BASE;

    if (digit < DECIMAL_BASE){
      digits[len++] = (char)('0' + digit);
    } else {
      digits[len++] = (char)((uppercase ? 'A' : 'a') + (digit - DECIMAL_BASE));
    }

    n /= HEX_BASE;
  }

  count = len;
  while (len != 0){
    putchar(digits[--len]);
  }
  
  // return number of characters printed
  return count;
}
