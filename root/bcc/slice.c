#include "../crt/ctype.h"
#include "../crt/stdio.h"
#include "../crt/print.h"
#include "../crt/stdbool.h"

#include "slice.h"
#include "arena.h"

// ASCII code used when printing escaped single quotes without relying on the
// bootstrap lexer's fragile handling of '\'' character literals.
#define K_ASCII_SINGLE_QUOTE 39

bool compare_slice_to_pointer(struct Slice* s, char *p) {
  for (size_t i = 0; i < s->len; i++) {
    if (p[i] != s->start[i])
      return false;
  }
  return p[s->len] == 0;
}

bool compare_slice_to_slice(struct Slice* self, struct Slice* other) {
  if (self->len != other->len)
    return false;
  for (size_t i = 0; i < self->len; i++) {
    if (other->start[i] != self->start[i])
      return false;
  }
  return true;
}

bool is_identifier(struct Slice* slice) {
  if (slice->len == 0)
    return false;
  if (!isalpha(slice->start[0]))
    return false;
  for (size_t i = 1; i < slice->len; i++)
    if (!isalnum(slice->start[i]))
      return false;
  return true;
}

struct Slice* slice_concat(struct Slice* a, char* b) {
  size_t b_len = 0;
  while (b[b_len] != 0) {
    b_len++;
  }

  char* new_str = (char*)arena_alloc(a->len + b_len);
  for (size_t i = 0; i < a->len; i++) {
    new_str[i] = a->start[i];
  }
  for (size_t i = 0; i < b_len; i++) {
    new_str[a->len + i] = b[i];
  }

  struct Slice* slice = (struct Slice*)arena_alloc(sizeof(struct Slice));
  slice->start = new_str;
  slice->len = a->len + b_len;
  return slice;
}

void print_slice(struct Slice* slice) {
  for (size_t i = 0; i < slice->len; i++) {
    putchar(slice->start[i]);
  }
}

void print_slice_with_escapes(struct Slice* slice) {
  for (size_t i = 0; i < slice->len; i++) {
    char c = slice->start[i];
    switch (c) {
      case '\a':
        putchar('\\');
        putchar('a');
        break;
      case '\b':
        putchar('\\');
        putchar('b');
        break;
      case '\f':
        putchar('\\');
        putchar('f');
        break;
      case '\n':
        putchar('\\');
        putchar('n');
        break;
      case '\r':
        putchar('\\');
        putchar('r');
        break;
      case '\t':
        putchar('\\');
        putchar('t');
        break;
      case '\v':
        putchar('\\');
        putchar('v');
        break;
      case '\\':
        putchar('\\');
        putchar('\\');
        break;
      case '\"':
        putchar('\\');
        putchar('"');
        break;
      case '\'':
        putchar('\\');
        putchar(K_ASCII_SINGLE_QUOTE);
        break;
      case '\0':
        putchar('\\');
        putchar('0');
        break;
      default:
        if (isprint((unsigned char)c)) {
          putchar(c);
        } else {
          int args[1];

          putchar('\\');
          args[0] = (unsigned char)c;
          fdprintf(STDOUT, "%03o", args);
        }
        break;
    }
  }
}

size_t hash_slice(struct Slice* key) {
  // djb2
  size_t out = 5381;
  for (size_t i = 0; i < key->len; i++) {
    char c = key->start[i];
    out = out * 33 + c;
  }
  return out;
}
