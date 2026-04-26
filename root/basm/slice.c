#include "../crt/print.h"
#include "../crt/ctype.h"

#include "slice.h"

bool compare_slice_to_pointer(struct Slice* s, char* p) {
  for (unsigned i = 0; i < s->len; i++) {
    if (p[i] != s->start[i]) return false;
  }
  return p[s->len] == 0;
}

bool compare_slice_to_slice(struct Slice* self, struct Slice* other) {
  if (self->len != other->len) return false;
  for (unsigned i = 0; i < self->len; i++) {
    if (other->start[i] != self->start[i]) return false;
  }
  return true;
}

bool is_identifier(struct Slice* slice) {
  if (slice->len == 0) return false;
  if (!isalpha(slice->start[0])) return false;
  for (unsigned i = 1; i < slice->len; i++) {
    if (!isalnum(slice->start[i])) return false;
  }
  return true;
}

void print_slice(struct Slice* slice) {
  for (unsigned i = 0; i < slice->len; i++) {
    putchar(slice->start[i]);
  }
}

void print_slice_err(struct Slice* slice) {
  print_slice(slice);
}

unsigned hash_slice(struct Slice* key) {
  unsigned out;

  out = 5381;
  for (unsigned i = 0; i < key->len; i++) {
    char c;
    c = key->start[i];
    out = out * 33 + (unsigned char)c;
  }
  return out;
}
