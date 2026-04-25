#include "print.h"
#include "unistd.h"

#define DECIMAL_BASE 10u
#define HEX_BASE 16u
#define MAX_INT_DEC_DIGITS 10 
#define MAX_SIGNED_DEC_CHARS 11
#define MAX_UNSIGNED_HEX_DIGITS 8 

// write() may complete only part of the requested block, so keep issuing calls
// until the full buffer has been consumed or the kernel reports failure.
static void write_fd_all(int fd, char* buf, unsigned count){
  int written;

  while (count != 0){
    written = write(fd, buf, count);
    if (written <= 0){
      return;
    }

    buf += (unsigned)written;
    count -= (unsigned)written;
  }
}

void putchar(char c){
  write_fd_all(STDOUT, &c, 1);
}

unsigned puts(char* str){
  return fdputs(STDOUT, str);
}

unsigned fdputs(int fd, char* str){
  char* start = str;
  unsigned count = 0;
  while(*str){
    ++str;
    ++count;
  }
  write_fd_all(fd, start, count);
  return count;
}

unsigned printf(char* fmt, void* arr){
  return fdprintf(STDOUT, fmt, arr);
}

static unsigned string_length(char* str){
  unsigned len = 0;
  while (str[len] != '\0') ++len;
  return len;
}

static unsigned min_unsigned(unsigned a, unsigned b){
  if (a < b) return a;
  return b;
}

static unsigned emit_padding(int fd, char pad, unsigned count){
  unsigned emitted = 0;
  while (count != 0){
    write_fd_all(fd, &pad, 1);
    ++emitted;
    --count;
  }
  return emitted;
}

static unsigned emit_span(int fd, char* start, unsigned len){
  write_fd_all(fd, start, len);
  return len;
}

static unsigned emit_unsigned_base(int fd, unsigned value, unsigned base, bool uppercase,
                                   unsigned min_width, bool zero_pad){
  char digits[MAX_INT_DEC_DIGITS];
  unsigned len = sizeof(digits);
  unsigned digit_count;
  unsigned digit;
  char pad_char = zero_pad ? '0' : ' ';

  if (value == 0){
    digits[--len] = '0';
  } else {
    while (value != 0){
      digit = value % base;
      if (digit < DECIMAL_BASE){
        digits[--len] = (char)('0' + digit);
      } else {
        digits[--len] = (char)((uppercase ? 'A' : 'a') + (digit - DECIMAL_BASE));
      }
      value /= base;
    }
  }

  digit_count = (unsigned)sizeof(digits) - len;
  if (min_width > digit_count){
    return emit_padding(fd, pad_char, min_width - digit_count) + emit_span(fd, &digits[len], digit_count);
  }
  return emit_span(fd, &digits[len], digit_count);
}

static unsigned emit_signed_base10(int fd, int value, unsigned min_width, bool zero_pad){
  char digits[MAX_SIGNED_DEC_CHARS];
  unsigned magnitude;
  unsigned len = MAX_SIGNED_DEC_CHARS;
  unsigned digit_count;
  char pad_char = zero_pad ? '0' : ' ';

  if (value == 0){
    digits[--len] = '0';
  } else {
    if (value < 0){
      magnitude = 0u - (unsigned)value;
    } else {
      magnitude = (unsigned)value;
    }

    while (magnitude != 0){
      digits[--len] = (char)('0' + (magnitude % DECIMAL_BASE));
      magnitude /= DECIMAL_BASE;
    }

    if (value < 0){
      digits[--len] = '-';
    }
  }

  digit_count = MAX_SIGNED_DEC_CHARS - len;
  if (min_width > digit_count){
    if (zero_pad && digits[len] == '-'){
      emit_span(fd, &digits[len], 1);
      return 1 + emit_padding(fd, '0', min_width - digit_count)
        + emit_span(fd, &digits[len + 1], digit_count - 1);
    }
    return emit_padding(fd, pad_char, min_width - digit_count) + emit_span(fd, &digits[len], digit_count);
  }
  return emit_span(fd, &digits[len], digit_count);
}

unsigned fdprintf(int fd, char* fmt, void* arr){
  unsigned count = 0;
  unsigned i = 0;
  char* literal_start;
  unsigned literal_len;
  unsigned* values = (unsigned*)arr;

  while (*fmt != '\0'){
    if (*fmt == '%'){
      bool zero_pad = false;
      unsigned min_width = 0;
      bool has_precision = false;
      unsigned precision = 0;

      ++count;
      ++fmt;
      if (*fmt == '0'){
        zero_pad = true;
        ++fmt;
      }
      while (*fmt >= '0' && *fmt <= '9'){
        min_width = (min_width * 10u) + (unsigned)(*fmt - '0');
        ++fmt;
      }
      if (*fmt == '.'){
        ++fmt;
        if (*fmt == '*'){
          has_precision = true;
          precision = values[i++];
          ++fmt;
        }
      }
      if (*fmt == 'l' || *fmt == 'z'){
        ++fmt;
      }

      if (*fmt == 'd'){
        count += emit_signed_base10(fd, ((int*)values)[i++], min_width, zero_pad);
        ++fmt;
        continue;
      } else if (*fmt == 'u'){
        count += emit_unsigned_base(fd, values[i++], DECIMAL_BASE, false, min_width, zero_pad);
        ++fmt;
        continue;
      } else if (*fmt == 'x'){
        count += emit_unsigned_base(fd, values[i++], HEX_BASE, false, min_width, zero_pad);
        ++fmt;
        continue;
      } else if (*fmt == 'X'){
        count += emit_unsigned_base(fd, values[i++], HEX_BASE, true, min_width, zero_pad);
        ++fmt;
        continue;
      } else if (*fmt == 's'){
        char* str = (char*)values[i++];
        unsigned len = string_length(str);
        if (has_precision){
          len = min_unsigned(len, precision);
        }
        count += emit_span(fd, str, len);
        ++fmt;
        continue;
      } else if (*fmt == 'c'){
        char c = (char)values[i++];
        write_fd_all(fd, &c, 1);
        ++count;
        ++fmt;
        continue;
      } else if (*fmt == '%'){
        write_fd_all(fd, "%", 1);
        ++count;
        ++fmt;
        continue;
      }

      // Unsupported format specifier: emit '%' literally and retry the current
      // character through the normal literal path on the next loop.
      write_fd_all(fd, "%", 1);
      ++count;
      continue;
    }

    // Emit literal spans in one block write instead of one syscall per byte.
    literal_start = fmt;
    literal_len = 0;
    while (*fmt != '\0' && *fmt != '%'){
      ++fmt;
      ++literal_len;
    }

    write_fd_all(fd, literal_start, literal_len);
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
  write_fd_all(STDOUT, &digits[len], count);
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
  write_fd_all(STDOUT, &digits[len], count);
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
  write_fd_all(STDOUT, &digits[len], count);
  return count;
}
