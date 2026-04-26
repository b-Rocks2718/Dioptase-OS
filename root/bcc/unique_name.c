#include "unique_name.h"

#include "arena.h"

// Purpose: Generate unique identifier and label slices.
// Inputs: Original names and suffixes are provided by the caller.
// Outputs: New slices are allocated in the arena.
// Invariants/Assumptions: unique_id_counter is incremented on each call.

// Purpose: Monotonic counter to keep generated names unique.
// Inputs: Incremented by make_unique and make_unique_label.
// Outputs: Used to format numeric suffixes.
// Invariants/Assumptions: Single-threaded use; not reset between functions.
static int unique_id_counter = 0;

// Purpose: Compute the decimal digit length of a counter value.
// Inputs: counter is the integer to measure.
// Outputs: Returns the number of digits needed in base-10.
// Invariants/Assumptions: counter is treated as non-negative.
unsigned counter_len(int counter) {
  unsigned len = 0;
  do {
    len++;
    counter /= 10;
  } while (counter != 0);
  return len;
}

// Purpose: Create a unique identifier by appending ".<id>".
// Inputs: original_name is the base identifier.
// Outputs: Returns a newly allocated Slice with the suffix applied.
// Invariants/Assumptions: Storage is arena-allocated and permanent.
struct Slice* make_unique(struct Slice* original_name) {
  unsigned id_len = counter_len(unique_id_counter);
  size_t new_len = original_name->len + 1 + id_len; // +1 for period

  char* new_str = (char*)arena_alloc(new_len);
  for (size_t i = 0; i < original_name->len; i++) {
    new_str[i] = original_name->start[i];
  }
  new_str[original_name->len] = '.';

  // append unique id
  int id = unique_id_counter;
  for (unsigned i = 0; i < id_len; i++) {
    new_str[new_len - 1 - i] = '0' + (id % 10);
    id /= 10;
  }

  unique_id_counter++;

  struct Slice* unique_name = (struct Slice*)arena_alloc(sizeof(struct Slice));
  unique_name->start = new_str;
  unique_name->len = new_len;

  return unique_name;
}

// Purpose: Create a unique function-scoped label with a suffix.
// Inputs: func_name is the function name; suffix is a label category string.
// Outputs: Returns a newly allocated Slice "func.suffix.<id>".
// Invariants/Assumptions: Storage is arena-allocated and permanent.
struct Slice* make_unique_label(struct Slice* func_name, char* suffix) {
  size_t suffix_len = 0;
  while (suffix[suffix_len] != '\0') {
    suffix_len++;
  }
  unsigned id_len = counter_len(unique_id_counter);
  size_t new_len = func_name->len + 1 + suffix_len + 1 + id_len; // +1 for period

  char* new_str = (char*)arena_alloc(new_len);
  for (size_t i = 0; i < func_name->len; i++) {
    new_str[i] = func_name->start[i];
  }
  new_str[func_name->len] = '.';
  for (size_t i = 0; i < suffix_len; i++) {
    new_str[func_name->len + 1 + i] = suffix[i];
  }

  new_str[func_name->len + 1 + suffix_len] = '.';

  // append unique id
  int id = unique_id_counter;
  for (unsigned i = 0; i < id_len; i++) {
    new_str[new_len - 1 - i] = '0' + (id % 10);
    id /= 10;
  }

  unique_id_counter++;

  struct Slice* unique_label = (struct Slice*)arena_alloc(sizeof(struct Slice));
  unique_label->start = new_str;
  unique_label->len = new_len;

  return unique_label;
}
