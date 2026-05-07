#ifndef SLICE_H
#define SLICE_H

#include "../crt/stdbool.h"
#include "../crt/stddef.h"

struct Slice {
  char * start; // where does the string start in memory?
  size_t len;        // How many characters in the string
};

bool compare_slice_to_pointer(struct Slice* s, char *p);

bool compare_slice_to_slice(struct Slice* self, struct Slice* other);

struct Slice* slice_concat(struct Slice* a, char* b);

bool is_identifier(struct Slice* slice);

void print_slice(struct Slice* slice);

void print_slice_with_escapes(struct Slice* slice);

size_t hash_slice(struct Slice* key);

#endif // SLICE_H
