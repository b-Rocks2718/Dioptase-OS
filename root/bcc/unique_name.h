#ifndef UNIQUE_NAME_H
#define UNIQUE_NAME_H

#include "slice.h"

// Purpose: Declare helpers for generating unique identifiers and labels.
// Inputs: Names are provided as slices or C strings.
// Outputs: Returns new slices with unique suffixes.
// Invariants/Assumptions: Uses a global counter for uniqueness.

// Purpose: Generate a unique identifier based on an existing name.
// Inputs: original_name is the user-provided identifier.
// Outputs: Returns a new Slice with ".<id>" appended.
// Invariants/Assumptions: Slice storage is arena-allocated and persistent.
struct Slice* make_unique(struct Slice* original_name);

// Purpose: Generate a unique label for a function-local label category.
// Inputs: func_name is the function name; suffix identifies label kind.
// Outputs: Returns a new Slice "func.suffix.<id>".
// Invariants/Assumptions: Slice storage is arena-allocated and persistent.
struct Slice* make_unique_label(struct Slice* func_name, char* suffix);

// Purpose: Compute the number of decimal digits needed for a counter.
// Inputs: counter is a non-negative integer.
// Outputs: Returns the decimal digit count.
// Invariants/Assumptions: counter is treated as base-10.
unsigned counter_len(int counter);

#endif // UNIQUE_NAME_H
