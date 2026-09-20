#ifndef UNIQUE_NAME_H
#define UNIQUE_NAME_H

#include "slice.h"

// Declare helpers for generating unique identifiers and labels.
// Returns new slices with unique suffixes.

// Generate a unique identifier based on an existing name.
// Returns a new Slice with ".<id>" appended.
struct Slice* make_unique(struct Slice* original_name);

// Generate a unique label for a function-local label category.
// Returns a new Slice "func.suffix.<id>".
struct Slice* make_unique_label(struct Slice* func_name, char* suffix);

// Compute the number of decimal digits needed for a counter.
// Returns the decimal digit count.
unsigned counter_len(int counter);

#endif // UNIQUE_NAME_H
