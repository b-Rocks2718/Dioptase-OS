#ifndef INSTRUCTION_ARRAY_H
#define INSTRUCTION_ARRAY_H

#include "../crt/stdbool.h"

struct InstructionArray {
  int origin;
  int* instructions;
  unsigned size;
  unsigned capacity;
  struct InstructionArray* next;
};

struct InstructionArrayList {
  struct InstructionArray* head;
  struct InstructionArray* tail;
};

struct InstructionArrayList* create_instruction_array_list(void);

void instruction_array_list_append(struct InstructionArrayList* list, struct InstructionArray* arr);

void destroy_instruction_array_list(struct InstructionArrayList* list);

void print_instruction_array_list(struct InstructionArrayList* list);

void fprint_instruction_array_list(int file, struct InstructionArrayList* list, bool raw);

// Purpose: Write instruction arrays as raw little-endian bytes.
// Inputs: ptr is the binary output; list is the instruction arrays; include_origin_padding
//         inserts zero bytes so each array begins at its origin address.
// Outputs: Writes raw bytes to ptr.
// Invariants/Assumptions: Origins are non-decreasing when include_origin_padding is true.
void fwrite_instruction_array_list(int file, struct InstructionArrayList* list, bool include_origin_padding);

struct InstructionArray* create_instruction_array(unsigned capacity, int origin);

// Purpose: Append a full 32-bit word to the instruction array.
// Inputs: arr is the destination array; value is the 32-bit word to append.
// Outputs: None.
// Invariants/Assumptions: arr is non-NULL and owned by the caller.
void instruction_array_append(struct InstructionArray* arr, int value);

// Purpose: Append a 16-bit value at the byte address pc, using little-endian byte order.
// Inputs: arr is the destination array; value is the 16-bit payload; pc is the absolute byte address.
// Outputs: None.
// Invariants/Assumptions: Calls are sequential in increasing pc.
void instruction_array_append_double(struct InstructionArray* arr, unsigned short value, int pc);

// Purpose: Append an 8-bit value at the byte address pc.
// Inputs: arr is the destination array; value is the 8-bit payload; pc is the absolute byte address.
// Outputs: None.
// Invariants/Assumptions: Calls are sequential in increasing pc.
void instruction_array_append_byte(struct InstructionArray* arr, char value, int pc);

int instruction_array_get(struct InstructionArray* arr, unsigned i);

void destroy_instruction_array(struct InstructionArray* arr);

void print_instruction_array(struct InstructionArray* arr);

void fprint_instruction_array(int file, struct InstructionArray* arr, bool raw);

unsigned instruction_array_list_size(struct InstructionArrayList* list);

#endif  // INSTRUCTION_ARRAY_H
