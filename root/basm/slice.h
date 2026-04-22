#ifndef SLICE_H
#define SLICE_H

#include "../crt/stdbool.h"

struct Slice {
  char* start; // where does the string start in memory?
  unsigned len;        // How many characters in the string?
};

bool compare_slice_to_pointer(struct Slice* s, char *p);

bool compare_slice_to_slice(struct Slice* self, struct Slice* other);

bool is_identifier(struct Slice* slice);

void print_slice(struct Slice* slice);

void print_slice_err(struct Slice* slice);

unsigned hash_slice(struct Slice* key);

#endif  // SLICE_H
