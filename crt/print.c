#include "print.h"
#include "sys.h"

#define DECIMAL_BASE 10u
#define HEX_BASE 16u
#define MAX_INT_DEC_DIGITS 10 
#define MAX_SIGNED_DEC_CHARS 11
#define MAX_UNSIGNED_HEX_DIGITS 8 

// write() may complete only part of the requested block, so keep issuing calls
// until the full buffer has been consumed or the kernel reports failure.
static void write_stdout_all(char* buf, unsigned count){
  int written;

  while (count != 0){
    written = write(STDOUT, buf, count);
    if (written <= 0){
      return;
    }

    buf += (unsigned)written;
    count -= (unsigned)written;
  }
}

void putchar(char c){
  write_stdout_all(&c, 1);
}

unsigned puts(char* str){
  char* start = str;
  unsigned count = 0;
  while(*str){
    ++str;
    ++count;
  }
  write_stdout_all(start, count);
  return count;
}

unsigned printf(char* fmt, void* arr){
  unsigned count = 0;
  unsigned i = 0;
  char* literal_start;
  unsigned literal_len;

  while (*fmt != '\0'){
    if (*fmt == '%'){
      if (*(fmt + 1) == 'd'){
        fmt += 2;
        count += print_signed(((int*)arr)[i++]);
        continue;
      } else if (*(fmt + 1) == 'u'){
        fmt += 2;
        count += print_unsigned(((unsigned*)arr)[i++]);
        continue;
      } else if (*(fmt + 1) == 'x'){
        fmt += 2;
        count += print_hex(((unsigned*)arr)[i++], false);
        continue;
      } else if (*(fmt + 1) == 'X'){
        fmt += 2;
        count += print_hex(((unsigned*)arr)[i++], true);
        continue;
      } else if (*(fmt + 1) == 's'){
        fmt += 2;
        count += puts((char*)((void**)arr)[i++]);
        continue;
      } else if (*(fmt + 1) == 'c'){
        fmt += 2;
        putchar(((unsigned*)arr)[i++]);
        ++count;
        continue;
      } else if (*(fmt + 1) == '%'){
        fmt += 2;
        putchar('%');
        ++count;
        continue;
      }

      // unsupported format specifier, print the '%' and let the next loop
      // iteration emit the following character unchanged.
      putchar(*fmt);
      ++count;
      ++fmt;
      continue;
    }

    // Emit literal spans in one block write instead of one syscall per byte.
    literal_start = fmt;
    literal_len = 0;
    while (*fmt != '\0' && *fmt != '%'){
      ++fmt;
      ++literal_len;
    }

    write_stdout_all(literal_start, literal_len);
    count += literal_len;
  }
  return count;
}

unsigned print_signed(int n){
  char digits[MAX_SIGNED_DEC_CHARS];
  unsigned magnitude;
  unsigned len = MAX_SIGNED_DEC_CHARS;
  unsigned count;

  if(n == 0){
    putchar('0');
    return 1;
  }

  if(n < 0){
    magnitude = 0u - (unsigned)n;
  } else {
    magnitude = (unsigned)n;
  }

  while (magnitude != 0){
    digits[--len] = (char)('0' + (magnitude % DECIMAL_BASE));
    magnitude /= DECIMAL_BASE;
  }

  if (n < 0){
    digits[--len] = '-';
  }

  count = MAX_SIGNED_DEC_CHARS - len;
  write_stdout_all(&digits[len], count);
  return count;
}

unsigned print_unsigned(unsigned n){
  char digits[MAX_INT_DEC_DIGITS];
  unsigned len = MAX_INT_DEC_DIGITS;
  unsigned count;

  if(n == 0){
    putchar('0');
    return 1;
  }

  while (n != 0){
    digits[--len] = (char)('0' + (n % DECIMAL_BASE));
    n /= DECIMAL_BASE;
  }

  count = MAX_INT_DEC_DIGITS - len;
  write_stdout_all(&digits[len], count);
  return count;
}

unsigned print_hex(unsigned n, bool uppercase){
  char digits[MAX_UNSIGNED_HEX_DIGITS];
  unsigned len = MAX_UNSIGNED_HEX_DIGITS;
  unsigned digit;
  unsigned count;

  if(n == 0){
    putchar('0');
    return 1;
  }

  while (n != 0){
    digit = n % HEX_BASE;

    if (digit < DECIMAL_BASE){
      digits[--len] = (char)('0' + digit);
    } else {
      digits[--len] = (char)((uppercase ? 'A' : 'a') + (digit - DECIMAL_BASE));
    }

    n /= HEX_BASE;
  }

  count = MAX_UNSIGNED_HEX_DIGITS - len;
  write_stdout_all(&digits[len], count);
  return count;
}
