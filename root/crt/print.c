#include "print.h"
#include "limits.h"
#include "unistd.h"

#define DECIMAL_BASE 10u
#define HEX_BASE 16u
#define MAX_INT_DEC_DIGITS 10
#define MAX_SIGNED_DEC_CHARS 11

/*
 * Purpose: Commit one complete byte span to a Dioptase descriptor.
 * Inputs: fd is the destination and buf owns at least count readable bytes.
 * Outputs: true only after all count bytes have been accepted by write().
 * Invariants/Assumptions: A positive short write commits that prefix, so the
 * next call starts immediately after it. A zero or negative result means no
 * forward progress and terminates the operation. The syscall contract never
 * returns more bytes than requested; reject such a result defensively rather
 * than underflowing count if that kernel invariant is ever broken.
 */
static bool write_fd_all(int fd, char* buf, unsigned count){
  int written;

  while (count != 0){
    written = write(fd, buf, count);
    if (written <= 0 || (unsigned)written > count){
      return false;
    }

    buf += (unsigned)written;
    count -= (unsigned)written;
  }
  return true;
}

int putchar(char c){
  if (!write_fd_all(STDOUT, &c, 1)){
    return -1;
  }
  return (unsigned char)c;
}

int puts(char* str){
  return fdputs(STDOUT, str);
}

int fdputs(int fd, char* str){
  int count = 0;

  if (str == NULL){
    return -1;
  }
  while (str[count] != '\0'){
    if (count == INT_MAX){
      return -1;
    }
    ++count;
  }
  if (!write_fd_all(fd, str, (unsigned)count)){
    return -1;
  }
  return count;
}

int printf(char* fmt, void* arr){
  return fdprintf(STDOUT, fmt, arr);
}

// Formatting results use int so every public routine has one failure sentinel.
// Refuse an unrepresentable byte count instead of wrapping it to a success.
static int string_output_length(char* str){
  int len = 0;

  if (str == NULL){
    return -1;
  }
  while (str[len] != '\0'){
    if (len == INT_MAX){
      return -1;
    }
    ++len;
  }
  return len;
}

static unsigned min_unsigned(unsigned a, unsigned b){
  if (a < b) return a;
  return b;
}

static bool add_emitted_count(int* total, int emitted){
  if (emitted < 0 || *total > INT_MAX - emitted){
    return false;
  }
  *total += emitted;
  return true;
}

static int emit_padding(int fd, char pad, unsigned count){
  int emitted = 0;

  if (count > (unsigned)INT_MAX){
    return -1;
  }
  while (count != 0){
    if (!write_fd_all(fd, &pad, 1)){
      return -1;
    }
    ++emitted;
    --count;
  }
  return emitted;
}

static int emit_span(int fd, char* start, unsigned len){
  if (len > (unsigned)INT_MAX || !write_fd_all(fd, start, len)){
    return -1;
  }
  return (int)len;
}

static int emit_unsigned_base(int fd, unsigned value, unsigned base,
                              bool uppercase, unsigned min_width,
                              bool zero_pad){
  char digits[MAX_INT_DEC_DIGITS];
  unsigned len = sizeof(digits);
  unsigned digit_count;
  unsigned digit;
  char pad_char = zero_pad ? '0' : ' ';
  int emitted;
  int result;

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
    emitted = emit_padding(fd, pad_char, min_width - digit_count);
    if (emitted < 0){
      return -1;
    }
    result = emit_span(fd, &digits[len], digit_count);
    if (result < 0){
      return -1;
    }
    return emitted + result;
  }
  return emit_span(fd, &digits[len], digit_count);
}

static int emit_signed_base10(int fd, int value, unsigned min_width,
                              bool zero_pad){
  char digits[MAX_SIGNED_DEC_CHARS];
  unsigned magnitude;
  unsigned len = MAX_SIGNED_DEC_CHARS;
  unsigned digit_count;
  char pad_char = zero_pad ? '0' : ' ';
  int emitted;
  int result;

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
      if (emit_span(fd, &digits[len], 1) < 0){
        return -1;
      }
      emitted = emit_padding(fd, '0', min_width - digit_count);
      if (emitted < 0){
        return -1;
      }
      result = emit_span(fd, &digits[len + 1], digit_count - 1);
      if (result < 0){
        return -1;
      }
      return 1 + emitted + result;
    }

    emitted = emit_padding(fd, pad_char, min_width - digit_count);
    if (emitted < 0){
      return -1;
    }
    result = emit_span(fd, &digits[len], digit_count);
    if (result < 0){
      return -1;
    }
    return emitted + result;
  }
  return emit_span(fd, &digits[len], digit_count);
}

int fdprintf(int fd, char* fmt, void* arr){
  int count = 0;
  unsigned i = 0;
  char* literal_start;
  unsigned literal_len;
  unsigned* values = (unsigned*)arr;
  int emitted;

  if (fmt == NULL){
    return -1;
  }

  while (*fmt != '\0'){
    if (*fmt == '%'){
      bool zero_pad = false;
      unsigned min_width = 0;
      bool has_precision = false;
      unsigned precision = 0;

      ++fmt;
      if (*fmt == '0'){
        zero_pad = true;
        ++fmt;
      }
      while (*fmt >= '0' && *fmt <= '9'){
        unsigned digit = (unsigned)(*fmt - '0');
        if (min_width > ((unsigned)INT_MAX - digit) / DECIMAL_BASE){
          return -1;
        }
        min_width = (min_width * DECIMAL_BASE) + digit;
        ++fmt;
      }
      if (*fmt == '.'){
        ++fmt;
        if (*fmt == '*'){
          if (values == NULL){
            return -1;
          }
          has_precision = true;
          precision = values[i++];
          ++fmt;
        }
      }
      if (*fmt == 'l' || *fmt == 'z'){
        ++fmt;
      }

      if (*fmt == 'd'){
        if (values == NULL){
          return -1;
        }
        emitted = emit_signed_base10(fd, ((int*)values)[i++], min_width,
                                     zero_pad);
        if (!add_emitted_count(&count, emitted)){
          return -1;
        }
        ++fmt;
        continue;
      } else if (*fmt == 'u'){
        if (values == NULL){
          return -1;
        }
        emitted = emit_unsigned_base(fd, values[i++], DECIMAL_BASE, false,
                                     min_width, zero_pad);
        if (!add_emitted_count(&count, emitted)){
          return -1;
        }
        ++fmt;
        continue;
      } else if (*fmt == 'x'){
        if (values == NULL){
          return -1;
        }
        emitted = emit_unsigned_base(fd, values[i++], HEX_BASE, false,
                                     min_width, zero_pad);
        if (!add_emitted_count(&count, emitted)){
          return -1;
        }
        ++fmt;
        continue;
      } else if (*fmt == 'X'){
        if (values == NULL){
          return -1;
        }
        emitted = emit_unsigned_base(fd, values[i++], HEX_BASE, true,
                                     min_width, zero_pad);
        if (!add_emitted_count(&count, emitted)){
          return -1;
        }
        ++fmt;
        continue;
      } else if (*fmt == 's'){
        int string_len;
        unsigned len;

        if (values == NULL){
          return -1;
        }
        char* str = (char*)values[i++];
        string_len = string_output_length(str);
        if (string_len < 0){
          return -1;
        }
        len = (unsigned)string_len;
        if (has_precision){
          len = min_unsigned(len, precision);
        }
        emitted = emit_span(fd, str, len);
        if (!add_emitted_count(&count, emitted)){
          return -1;
        }
        ++fmt;
        continue;
      } else if (*fmt == 'c'){
        if (values == NULL){
          return -1;
        }
        char c = (char)values[i++];
        emitted = emit_span(fd, &c, 1);
        if (!add_emitted_count(&count, emitted)){
          return -1;
        }
        ++fmt;
        continue;
      } else if (*fmt == '%'){
        emitted = emit_span(fd, "%", 1);
        if (!add_emitted_count(&count, emitted)){
          return -1;
        }
        ++fmt;
        continue;
      }

      // Unsupported format specifier: emit '%' literally and retry the current
      // character through the normal literal path on the next loop.
      emitted = emit_span(fd, "%", 1);
      if (!add_emitted_count(&count, emitted)){
        return -1;
      }
      continue;
    }

    // Emit literal spans in one block write instead of one syscall per byte.
    literal_start = fmt;
    literal_len = 0;
    while (*fmt != '\0' && *fmt != '%'){
      ++fmt;
      ++literal_len;
    }

    emitted = emit_span(fd, literal_start, literal_len);
    if (!add_emitted_count(&count, emitted)){
      return -1;
    }
  }
  return count;
}

int print_signed(int n){
  return emit_signed_base10(STDOUT, n, 0, false);
}

int print_unsigned(unsigned n){
  return emit_unsigned_base(STDOUT, n, DECIMAL_BASE, false, 0, false);
}

int print_hex(unsigned n, bool uppercase){
  return emit_unsigned_base(STDOUT, n, HEX_BASE, uppercase, 0, false);
}
