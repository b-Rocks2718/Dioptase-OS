#include "../crt/assert.h"
#include "../crt/print.h"
#include "../crt/stdlib.h"
#include "../crt/stddef.h"
#include "../crt/stdint.h"
#include "../crt/unistd.h"
#include "instruction_array.h"

#define kWordBytes 4u
#define kByteMask 0xFFu
#define kByteStride 1u

/*
  Linked list for holding instruction arrays
*/

struct InstructionArrayList* create_instruction_array_list(void){
  struct InstructionArrayList* list = malloc(sizeof(struct InstructionArrayList));
  list->head = create_instruction_array(10, 0);
  list->tail = list->head;
  return list;
}

// Link an instruction array at the end of the origin-ordered list.
void instruction_array_list_append(struct InstructionArrayList* list, struct InstructionArray* arr){
  if (list->head == NULL){
    list->head = arr;
    list->tail = arr;
  } else {
    assert(list->tail != NULL, "instruction array list tail missing");
    list->tail->next = arr;
    list->tail = arr;
  }
}

// Free all instruction-array nodes and their list wrapper.
void destroy_instruction_array_list(struct InstructionArrayList* list){
  destroy_instruction_array(list->head);
  free(list);
}

// Print every instruction array using the human-readable formatter.
void print_instruction_array_list(struct InstructionArrayList* list){
  print_instruction_array(list->head);
}

// Write every instruction array using the selected raw/debug format.
void fprint_instruction_array_list(int file, struct InstructionArrayList* list, bool raw){
  fprint_instruction_array(file, list->head, raw);
}

// Write one instruction word as four little-endian bytes.
// Ptr is open for binary output.
static void write_all(int file, uint8_t* bytes, size_t len){
  size_t offset = 0;
  while (offset < len){
    int wrote = write(file, bytes + offset, len - offset);
    assert(wrote >= 0, "instruction array write failed");
    offset += (size_t)wrote;
  }
}

// Encode and write one unsigned 32-bit little-endian value.
static void write_u32_le(int file, uint32_t value){
  uint8_t bytes[kWordBytes];
  bytes[0] = (uint8_t)(value & kByteMask);
  bytes[1] = (uint8_t)((value >> 8) & kByteMask);
  bytes[2] = (uint8_t)((value >> 16) & kByteMask);
  bytes[3] = (uint8_t)((value >> 24) & kByteMask);
  write_all(file, bytes, kWordBytes);
}

// Emit zero padding bytes.
// Ptr is open for binary output.
static void write_zero_bytes(int file, size_t count){
  #define kZeroChunkBytes 256u
  static uint8_t zeros[kZeroChunkBytes] = {0};
  while (count > 0){
    size_t chunk = count > kZeroChunkBytes ? kZeroChunkBytes : count;
    write_all(file, zeros, chunk);
    count -= chunk;
  }
}

// Serialize all words in one instruction array without padding.
// Ptr is open for binary output.
static void write_instruction_array_words(int file, struct InstructionArray* arr){
  for (size_t i = 0; i < arr->size; ++i){
    write_u32_le(file, (uint32_t)arr->instructions[i]);
  }
}

// Serialize arrays in order, optionally inserting origin gaps as zero bytes.
void fwrite_instruction_array_list(int file, struct InstructionArrayList* list, bool include_origin_padding){
  uint32_t cursor = 0;
  for (struct InstructionArray* arr = list->head; arr != NULL; arr = arr->next){
    uint32_t origin = (uint32_t)arr->origin;
    if (include_origin_padding){
      assert(origin >= cursor, "instruction array origins must be non-decreasing");
      if (origin > cursor){
        write_zero_bytes(file, (size_t)(origin - cursor));
      }
      cursor = origin;
    }
    write_instruction_array_words(file, arr);
    cursor += (uint32_t)(arr->size * kWordBytes);
  }
}

/*
  Dynamic array used for holding instructions
*/

struct InstructionArray* create_instruction_array(unsigned capacity, int origin){
  int* instructions = malloc(sizeof(int) * capacity);

  struct InstructionArray* arr = malloc(sizeof(struct InstructionArray));

  arr->capacity = capacity;
  arr->size = 0;
  arr->instructions = instructions;
  arr->origin = origin;
  arr->next = NULL;

  return arr;
}

// Double an instruction array's storage while retaining existing words.
static int* grow_instruction_storage(int* old_instructions, unsigned old_capacity){
  unsigned new_capacity = old_capacity * 2;
  int* new_instructions = malloc(sizeof(int) * new_capacity);
  for (unsigned i = 0; i < old_capacity; ++i){
    new_instructions[i] = old_instructions[i];
  }
  free(old_instructions);
  return new_instructions;
}

// Append one machine word, growing storage when the array is full.
void instruction_array_append(struct InstructionArray* arr, int value){
  if (arr->size == arr->capacity){
    arr->instructions = grow_instruction_storage(arr->instructions, arr->capacity);
    arr->capacity = arr->capacity * 2;
  } 
    
  arr->instructions[arr->size] = value;
  arr->size++;
}

// Update a byte within an existing 32-bit word.
// Word points to the word to update; byte_index is 0..3; value is the byte payload.
static void set_word_byte(int* word, int byte_index, uint8_t value){
  uint32_t mask = (uint32_t)kByteMask << (8 * byte_index);
  uint32_t updated = ((uint32_t)(*word) & ~mask) | ((uint32_t)value << (8 * byte_index));
  *word = (int)updated;
}

// Append a 16-bit value at the requested byte address.
void instruction_array_append_double(struct InstructionArray* arr, unsigned short value, int pc){
  // Little-endian: low byte goes at the lowest address.
  instruction_array_append_byte(arr, (uint8_t)(value & kByteMask), pc);
  instruction_array_append_byte(arr, (uint8_t)((value >> 8) & kByteMask), pc + kByteStride);
}

// Append an 8-bit value at the requested byte address.
void instruction_array_append_byte(struct InstructionArray* arr, char value, int pc){
  int byte_index = pc % kWordBytes;
  if (byte_index == 0){
    instruction_array_append(arr, 0);
  }
  assert(arr->size > 0, "byte append requires an allocated word");
  set_word_byte(&arr->instructions[arr->size - 1], byte_index, value);
}

// Read the byte-addressed word containing the requested program counter.
int instruction_array_get(struct InstructionArray* arr, unsigned i){
  // no checks on i, might regret this later
  return arr->instructions[i];
}

// Recursively free an instruction-array node and its successors.
void destroy_instruction_array(struct InstructionArray* arr){
  if (arr->next != NULL) destroy_instruction_array(arr->next);
  free(arr->instructions);
  free(arr);
}

// Print one instruction array in numeric word form.
void print_instruction_array(struct InstructionArray* arr){
  int header_args[1];
  header_args[0] = arr->origin;
  printf("@%d\n", header_args);
  for (int i = 0; i < arr->size; ++i){
    int args[1];
    args[0] = arr->instructions[i];
    printf("%08X\n", args);
  }
  if (arr->next != NULL) print_instruction_array(arr->next);
}

// Write one instruction array as raw words or formatted text.
void fprint_instruction_array(int file, struct InstructionArray* arr, bool raw){
  // raw => no ELF structure => put origin markers
  if (raw) {
    int args[1];
    args[0] = arr->origin / 4;
    fdprintf(file, "@%X\n", args);
  }
  for (int i = 0; i < arr->size; ++i){
    int args[1];
    args[0] = arr->instructions[i];
    fdprintf(file, "%08X\n", args);
  }
  if (arr->next != NULL) fprint_instruction_array(file, arr->next, raw);
}

// Return the total serialized byte size of all arrays in the list.
unsigned instruction_array_list_size(struct InstructionArrayList* list){
  unsigned total_size = 0;
  struct InstructionArray* curr = list->head;
  while (curr != NULL){
    total_size += curr->size;
    curr = curr->next;
  }
  return total_size;
}
