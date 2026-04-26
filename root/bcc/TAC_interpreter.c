#include "TAC.h"
#include "slice.h"

#include "../crt/stdbool.h"
#include "../crt/stdint.h"
#include "../crt/stdarg.h"
#include "../crt/stdio.h"
#include "../crt/print.h"
#include "../crt/stdlib.h"
#include "../crt/string.h"
#include "../crt/limits.h"

// TAC interpreter written by Codex

// Purpose: Provide a small TAC interpreter for validating TAC lowering output.
// Inputs: Consumes a TACProg with functions and static variables.
// Outputs: Returns the integer result of main() or exits on interpreter errors.
// Invariants/Assumptions: Values are normalized to 32/64-bit based on type metadata.

// Purpose: Define interpreter memory sizing constants.
// Inputs: Used for address allocation and dynamic array growth.
// Outputs: Controls initial capacities, growth behavior, and scalar slot counts.
// Invariants/Assumptions: Values are stored in 4-byte slots and grow by factor 2.
static int kTacInterpWordBytes = 4;
static size_t kTacInterpInitialMemoryCapacity = 8;
static size_t kTacInterpInitialBindingCapacity = 8;
static size_t kTacInterpInitialLabelCapacity = 8;
static size_t kTacInterpGrowthFactor = 2;
static size_t kTacInterpSingleSlot = 1;
static size_t kTacInterpMainNameLen = sizeof("main") - 1;
static int kTacInterpFunctionAddrBase = 0x10000000;
// Purpose: Define supported host builtin names and signatures.
// Inputs/Outputs: Used to detect external calls like putchar.
// Invariants/Assumptions: Builtins exist only for test-visible output.
static char* kTacBuiltinPutcharName = "putchar";
static size_t kTacBuiltinPutcharArgCount = 1;

// Purpose: Track one addressable memory cell in the interpreter.
// Inputs: address is a byte address; value is the stored integer.
// Outputs: initialized indicates whether the cell has a defined value.
// Invariants/Assumptions: address values are byte addresses; allocations align to kTacInterpWordBytes.
struct TacMemoryCell {
  int address;
  uint64_t value;
  bool initialized;
};

// Purpose: Maintain the interpreter's memory space.
// Inputs: cells holds allocated memory slots; next_address is the allocator cursor.
// Outputs: Stores variable and pointer-referenced values.
// Invariants/Assumptions: Addresses are monotonically allocated.
struct TacMemory {
  struct TacMemoryCell* cells;
  size_t count;
  size_t capacity;
  int next_address;
};

// Purpose: Map an identifier name to a memory address.
// Inputs: name is the variable identifier; address is its storage base.
// Outputs: Used for variable lookup and address-of operations.
// Invariants/Assumptions: name pointers remain valid for the interpreter lifetime.
struct TacBinding {
  struct Slice* name;
  int address;
};

// Purpose: Maintain name-to-address bindings for a scope.
// Inputs: bindings stores the list of bound identifiers.
// Outputs: Provides lookup for globals and locals.
// Invariants/Assumptions: Names are unique within a binding set.
struct TacBindings {
  struct TacBinding* bindings;
  size_t count;
  size_t capacity;
};

// Purpose: Track required storage for CopyTo/FromOffset base variables.
// Inputs: name is the variable name; bytes is the max byte span needed.
// Outputs: Used to pre-allocate local storage before executing a function.
// Invariants/Assumptions: bytes counts from offset 0 up to the highest accessed byte.
struct TacCopyOffsetRequirement {
  struct Slice* name;
  size_t bytes;
};

// Purpose: Hold a dynamic list of CopyTo/FromOffset storage requirements.
// Inputs/Outputs: entries stores the requirements; count/capacity track usage.
// Invariants/Assumptions: Names are unique within the map.
struct TacCopyOffsetMap {
  struct TacCopyOffsetRequirement* entries;
  size_t count;
  size_t capacity;
};

// Purpose: Map a function name to a synthetic address for function pointers.
// Inputs: name is the function identifier; func is the top-level node; address is the pointer value.
// Outputs: Used to resolve function-pointer calls and address-of operations.
// Invariants/Assumptions: address values are unique and non-zero.
struct TacFunctionEntry {
  struct Slice* name;
  struct TopLevel* func;
  int address;
};

// Purpose: Track function pointer addresses for the interpreter.
// Inputs/Outputs: entries holds the registered functions; next_address allocates unique addresses.
// Invariants/Assumptions: next_address is advanced by word size for each function.
struct TacFunctionTable {
  struct TacFunctionEntry* entries;
  size_t count;
  size_t capacity;
  int next_address;
};

// Purpose: Associate a label name with its instruction node.
// Inputs: label is the label name; instr points at the label instruction.
// Outputs: Supports TACJUMP and TACCOND_JUMP dispatch.
// Invariants/Assumptions: Label names are unique within a function body.
struct TacLabelEntry {
  struct Slice* label;
  struct TACInstr* instr;
};

// Purpose: Store per-function execution state for the interpreter.
// Inputs: locals holds the local bindings; labels index jump targets.
// Outputs: Tracks comparison state for TACCOND_JUMP.
// Invariants/Assumptions: cmp_valid is set only after a TACCMP.
struct TacFrame {
  struct TacBindings locals;
  struct TacLabelEntry* labels;
  size_t label_count;
  size_t label_capacity;
  bool cmp_valid;
  uint64_t cmp_left;
  uint64_t cmp_right;
};

// Purpose: Hold interpreter-wide state across function calls.
// Inputs: prog is the TAC program; memory and globals are initialized up front.
// Outputs: Provides global storage and shared memory space.
// Invariants/Assumptions: Globals are initialized before executing main.
struct TacInterpreter {
  struct TACProg* prog;
  struct TacMemory memory;
  struct TacBindings globals;
  struct TacFunctionTable functions;
};

// Purpose: Emit a TAC interpreter error and terminate execution.
// Inputs: fmt is a printf-style format string.
// Outputs: Writes to stderr and exits with non-zero status.
// Invariants/Assumptions: Used for irrecoverable interpreter errors.
static void tac_interp_error(char* fmt, ...) {
  va_list args;
  fprintf(stderr, "TAC Interpreter Error: ");
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fprintf(stderr, "\n");
  exit(EXIT_FAILURE);
}

// Purpose: Initialize a TacMemory structure.
// Inputs: mem points to the memory object to initialize.
// Outputs: Resets memory tracking to an empty state.
// Invariants/Assumptions: mem is non-NULL.
static void tac_memory_init(struct TacMemory* mem) {
  mem->cells = NULL;
  mem->count = 0;
  mem->capacity = 0;
  // Reserve address 0 to represent the null pointer.
  mem->next_address = kTacInterpWordBytes;
}

// Purpose: Release a TacMemory's allocated resources.
// Inputs: mem points to the memory object to free.
// Outputs: Frees allocated memory slots.
// Invariants/Assumptions: mem was initialized with tac_memory_init.
static void tac_memory_destroy(struct TacMemory* mem) {
  free(mem->cells);
  mem->cells = NULL;
  mem->count = 0;
  mem->capacity = 0;
  mem->next_address = 0;
}

// Purpose: Grow the TacMemory cell array if needed.
// Inputs: mem points to the memory object to grow.
// Outputs: Ensures capacity for at least one more cell.
// Invariants/Assumptions: mem is non-NULL.
static void tac_memory_reserve(struct TacMemory* mem) {
  if (mem->count < mem->capacity) {
    return;
  }
  size_t new_capacity = (mem->capacity == 0)
                            ? kTacInterpInitialMemoryCapacity
                            : mem->capacity * kTacInterpGrowthFactor;
  struct TacMemoryCell* next_cells =
      (struct TacMemoryCell*)realloc(mem->cells, new_capacity * sizeof(struct TacMemoryCell));
  if (next_cells == NULL) {
    tac_interp_error("memory allocation failed while growing TAC memory");
  }
  mem->cells = next_cells;
  mem->capacity = new_capacity;
}

// Purpose: Find an existing memory cell by address.
// Inputs: mem is the memory object; address is the byte address to find.
// Outputs: Returns the cell pointer or NULL if not found.
// Invariants/Assumptions: mem is initialized.
static struct TacMemoryCell* tac_memory_find_cell(struct TacMemory* mem, int address) {
  for (size_t i = 0; i < mem->count; i++) {
    if (mem->cells[i].address == address) {
      return &mem->cells[i];
    }
  }
  return NULL;
}

// Purpose: Ensure a memory cell exists for a given address.
// Inputs: mem is the memory object; address is the byte address to access.
// Outputs: Returns a pointer to the memory cell, creating it if missing.
// Invariants/Assumptions: New cells are marked uninitialized.
static struct TacMemoryCell* tac_memory_get_cell(struct TacMemory* mem, int address) {
  struct TacMemoryCell* cell = tac_memory_find_cell(mem, address);
  if (cell != NULL) {
    return cell;
  }
  tac_memory_reserve(mem);
  cell = &mem->cells[mem->count++];
  cell->address = address;
  cell->value = 0;
  cell->initialized = false;
  return cell;
}

// Purpose: Allocate a contiguous range of memory slots.
// Inputs: mem is the memory object; slots is the number of word slots to reserve.
// Outputs: Returns the base byte address for the allocated range.
// Invariants/Assumptions: slots must be non-zero.
static int tac_memory_alloc_range(struct TacMemory* mem, size_t slots) {
  if (slots == 0) {
    tac_interp_error("attempted to allocate zero TAC memory slots");
  }
  int base = mem->next_address;
  for (size_t i = 0; i < slots; i++) {
    int address = base + (int)(i * kTacInterpWordBytes);
    (void)tac_memory_get_cell(mem, address);
  }
  mem->next_address += (int)(slots * kTacInterpWordBytes);
  return base;
}

// Purpose: Store a value into a memory cell.
// Inputs: mem is the memory object; address is the target byte address; value is the data.
// Outputs: Writes the value and marks the cell initialized.
// Invariants/Assumptions: address refers to a word slot.
static void tac_memory_store(struct TacMemory* mem, int address, uint64_t value) {
  struct TacMemoryCell* cell = tac_memory_get_cell(mem, address);
  cell->value = value;
  cell->initialized = true;
}

// Purpose: Load a value from a memory cell.
// Inputs: mem is the memory object; address is the source byte address.
// Outputs: Returns the stored value.
// Invariants/Assumptions: Loading an uninitialized address is an error.
static uint64_t tac_memory_load(struct TacMemory* mem, int address) {
  struct TacMemoryCell* cell = tac_memory_find_cell(mem, address);
  if (cell == NULL || !cell->initialized) {
    tac_interp_error("load from uninitialized address %d", address);
  }
  return cell->value;
}

// Purpose: Initialize a TacBindings structure.
// Inputs: bindings points to the bindings object to initialize.
// Outputs: Resets the binding list to empty.
// Invariants/Assumptions: bindings is non-NULL.
static void tac_bindings_init(struct TacBindings* bindings) {
  bindings->bindings = NULL;
  bindings->count = 0;
  bindings->capacity = 0;
}

// Purpose: Release memory held by a TacBindings structure.
// Inputs: bindings points to the bindings object to free.
// Outputs: Frees the bindings array.
// Invariants/Assumptions: bindings was initialized with tac_bindings_init.
static void tac_bindings_destroy(struct TacBindings* bindings) {
  free(bindings->bindings);
  bindings->bindings = NULL;
  bindings->count = 0;
  bindings->capacity = 0;
}

// Purpose: Grow a binding list if needed.
// Inputs: bindings points to the bindings object to grow.
// Outputs: Ensures capacity for at least one more binding.
// Invariants/Assumptions: bindings is non-NULL.
static void tac_bindings_reserve(struct TacBindings* bindings) {
  if (bindings->count < bindings->capacity) {
    return;
  }
  size_t new_capacity = (bindings->capacity == 0)
                            ? kTacInterpInitialBindingCapacity
                            : bindings->capacity * kTacInterpGrowthFactor;
  struct TacBinding* next =
      (struct TacBinding*)realloc(bindings->bindings, new_capacity * sizeof(struct TacBinding));
  if (next == NULL) {
    tac_interp_error("memory allocation failed while growing TAC bindings");
  }
  bindings->bindings = next;
  bindings->capacity = new_capacity;
}

// Purpose: Find an existing binding by name.
// Inputs: bindings is the binding list; name is the identifier to find.
// Outputs: Returns the binding pointer or NULL if not found.
// Invariants/Assumptions: Name comparisons use slice equality.
static struct TacBinding* tac_bindings_find(struct TacBindings* bindings, struct Slice* name) {
  for (size_t i = 0; i < bindings->count; i++) {
    if (compare_slice_to_slice(bindings->bindings[i].name, name)) {
      return &bindings->bindings[i];
    }
  }
  return NULL;
}

// Purpose: Create or return a binding with a specific slot allocation.
// Inputs: bindings is the binding list; mem is the shared memory allocator; name is the identifier.
// Outputs: Returns the binding for the identifier, allocating storage if missing.
// Invariants/Assumptions: slots must be non-zero.
static struct TacBinding* tac_bindings_get_or_add_range(struct TacBindings* bindings,
                                                        struct TacMemory* mem,
                                                        struct Slice* name,
                                                        size_t slots) {
  struct TacBinding* found = tac_bindings_find(bindings, name);
  if (found != NULL) {
    return found;
  }
  tac_bindings_reserve(bindings);
  struct TacBinding* binding = &bindings->bindings[bindings->count++];
  binding->name = name;
  binding->address = tac_memory_alloc_range(mem, slots);
  return binding;
}

// Purpose: Create or return a binding for a name.
// Inputs: bindings is the binding list; mem is the shared memory allocator; name is the identifier.
// Outputs: Returns the binding for the identifier, allocating storage if missing.
// Invariants/Assumptions: Newly created bindings allocate one word slot.
static struct TacBinding* tac_bindings_get_or_add(struct TacBindings* bindings,
                                                  struct TacMemory* mem,
                                                  struct Slice* name) {
  return tac_bindings_get_or_add_range(bindings, mem, name, kTacInterpSingleSlot);
}

// Purpose: Convert a byte span into the number of word slots required.
// Inputs: bytes is the total byte count to cover.
// Outputs: Returns the number of word slots to reserve.
// Invariants/Assumptions: bytes is non-zero and slots are 4-byte words.
static size_t tac_slots_for_bytes(size_t bytes) {
  if (bytes == 0) {
    tac_interp_error("zero-sized copy offset allocation");
  }
  size_t slots = (bytes + (size_t)kTacInterpWordBytes - 1) / (size_t)kTacInterpWordBytes;
  return (slots > 0) ? slots : kTacInterpSingleSlot;
}

// Purpose: Initialize a CopyTo/FromOffset requirement map.
// Inputs: map is the map to initialize.
// Outputs: Resets the map to an empty state.
// Invariants/Assumptions: map is non-NULL.
static void tac_copy_offset_map_init(struct TacCopyOffsetMap* map) {
  map->entries = NULL;
  map->count = 0;
  map->capacity = 0;
}

// Purpose: Release resources held by a CopyTo/FromOffset requirement map.
// Inputs: map is the map to destroy.
// Outputs: Frees map storage and resets metadata.
// Invariants/Assumptions: map was initialized with tac_copy_offset_map_init.
static void tac_copy_offset_map_destroy(struct TacCopyOffsetMap* map) {
  free(map->entries);
  map->entries = NULL;
  map->count = 0;
  map->capacity = 0;
}

// Purpose: Grow the CopyTo/FromOffset requirement map if needed.
// Inputs: map is the map to grow.
// Outputs: Ensures capacity for one additional entry.
// Invariants/Assumptions: map is non-NULL.
static void tac_copy_offset_map_reserve(struct TacCopyOffsetMap* map) {
  if (map->count < map->capacity) {
    return;
  }
  size_t new_capacity = (map->capacity == 0)
                            ? kTacInterpInitialBindingCapacity
                            : map->capacity * kTacInterpGrowthFactor;
  struct TacCopyOffsetRequirement* next =
      (struct TacCopyOffsetRequirement*)realloc(map->entries,
                                                new_capacity * sizeof(struct TacCopyOffsetRequirement));
  if (next == NULL) {
    tac_interp_error("memory allocation failed while growing copy offset map");
  }
  map->entries = next;
  map->capacity = new_capacity;
}

// Purpose: Find an existing CopyTo/FromOffset requirement by name.
// Inputs: map is the requirement map; name is the variable name.
// Outputs: Returns the entry pointer or NULL if not found.
// Invariants/Assumptions: Name comparisons use slice equality.
static struct TacCopyOffsetRequirement* tac_copy_offset_map_find(struct TacCopyOffsetMap* map,
                                                                 struct Slice* name) {
  for (size_t i = 0; i < map->count; i++) {
    if (compare_slice_to_slice(map->entries[i].name, name)) {
      return &map->entries[i];
    }
  }
  return NULL;
}

// Purpose: Record the maximum byte span required for a CopyTo/FromOffset base.
// Inputs: map is the requirement map; name is the variable name; bytes is required span.
// Outputs: Updates or inserts the requirement entry for the name.
// Invariants/Assumptions: bytes counts from offset 0 and fits in size_t.
static void tac_copy_offset_map_update(struct TacCopyOffsetMap* map,
                                       struct Slice* name,
                                       size_t bytes) {
  if (name == NULL) {
    tac_interp_error("copy offset requirement missing base name");
  }
  struct TacCopyOffsetRequirement* entry = tac_copy_offset_map_find(map, name);
  if (entry != NULL) {
    if (bytes > entry->bytes) {
      entry->bytes = bytes;
    }
    return;
  }
  tac_copy_offset_map_reserve(map);
  map->entries[map->count].name = name;
  map->entries[map->count].bytes = bytes;
  map->count++;
}

// Purpose: Compute the byte span required for a CopyTo/FromOffset access.
// Inputs: op_name labels the operation; offset is the byte offset; type describes the value size.
// Outputs: Returns the byte span needed to cover the access.
// Invariants/Assumptions: offset is non-negative and type has a non-zero size.
static size_t tac_copy_offset_required_bytes(char* op_name,
                                             int offset,
                                             struct Type* type,
                                             struct Slice* base_name) {
  if (offset < 0) {
    tac_interp_error("%s uses negative offset %d", op_name, offset);
  }
  if (type == NULL) {
    tac_interp_error("%s missing type information", op_name);
  }
  size_t size = get_type_size((struct Type*)type);
  if (size == 0) {
    tac_interp_error("%s has zero-sized type", op_name);
  }
  size_t base = (size_t)offset;
  if (size > SIZE_MAX - base) {
    tac_interp_error("%s size overflow for %.*s",
                     op_name,
                     base_name ? (int)base_name->len : 0,
                     base_name ? base_name->start : "<unknown>");
  }
  return base + size;
}

// Purpose: Collect CopyTo/FromOffset storage requirements for a TAC function body.
// Inputs: map is the requirement map; body is the TAC instruction list.
// Outputs: Populates map with max byte spans for each base variable.
// Invariants/Assumptions: TAC instruction list links are acyclic.
static void tac_collect_copy_offset_requirements(struct TacCopyOffsetMap* map,
                                                 struct TACInstr* body) {
  for (struct TACInstr* cur = body; cur != NULL; cur = cur->next) {
    switch (cur->type) {
      case TACCOPY_TO_OFFSET: {
        struct Slice* base = cur->instr.tac_copy_to_offset.dst;
        struct Val* src = cur->instr.tac_copy_to_offset.src;
        struct Type* dst_type = cur->instr.tac_copy_to_offset.dst_type;
        size_t bytes = tac_copy_offset_required_bytes("copy-to-offset",
                                                      cur->instr.tac_copy_to_offset.offset,
                                                      dst_type != NULL ? dst_type : (src ? src->type : NULL),
                                                      base);
        tac_copy_offset_map_update(map, base, bytes);
        break;
      }
      case TACCOPY_FROM_OFFSET: {
        struct Slice* base = cur->instr.tac_copy_from_offset.src;
        struct Val* dst = cur->instr.tac_copy_from_offset.dst;
        size_t bytes = tac_copy_offset_required_bytes("copy-from-offset",
                                                      cur->instr.tac_copy_from_offset.offset,
                                                      dst ? dst->type : NULL,
                                                      base);
        tac_copy_offset_map_update(map, base, bytes);
        break;
      }
      default:
        break;
    }
  }
}

// Purpose: Pre-allocate local storage for CopyTo/FromOffset base variables.
// Inputs: frame is the current frame; interp is the interpreter state; body is the TAC list.
// Outputs: Ensures local bindings exist for each CopyTo/FromOffset base variable.
// Invariants/Assumptions: Globals are allocated separately before function execution.
static void tac_preallocate_copy_offsets(struct TacFrame* frame,
                                         struct TacInterpreter* interp,
                                         struct TACInstr* body) {
  struct TacCopyOffsetMap map;
  tac_copy_offset_map_init(&map);
  tac_collect_copy_offset_requirements(&map, body);

  for (size_t i = 0; i < map.count; i++) {
    struct Slice* name = map.entries[i].name;
    if (name == NULL) {
      continue;
    }
    if (tac_bindings_find(&interp->globals, name) != NULL) {
      continue;
    }
    size_t slots = tac_slots_for_bytes(map.entries[i].bytes);
    (void)tac_bindings_get_or_add_range(&frame->locals, &interp->memory, name, slots);
  }

  tac_copy_offset_map_destroy(&map);
}

// Purpose: Initialize a TacFunctionTable structure.
// Inputs: table points to the function table to initialize.
// Outputs: Resets the table to empty and sets the first synthetic address.
// Invariants/Assumptions: table is non-NULL.
static void tac_function_table_init(struct TacFunctionTable* table) {
  table->entries = NULL;
  table->count = 0;
  table->capacity = 0;
  table->next_address = kTacInterpFunctionAddrBase;
}

// Purpose: Release memory held by a TacFunctionTable.
// Inputs: table points to the function table to free.
// Outputs: Frees the entries array and resets metadata.
// Invariants/Assumptions: table was initialized with tac_function_table_init.
static void tac_function_table_destroy(struct TacFunctionTable* table) {
  free(table->entries);
  table->entries = NULL;
  table->count = 0;
  table->capacity = 0;
  table->next_address = 0;
}

// Purpose: Grow the function table if needed.
// Inputs: table points to the function table to grow.
// Outputs: Ensures capacity for at least one more entry.
// Invariants/Assumptions: table is non-NULL.
static void tac_function_table_reserve(struct TacFunctionTable* table) {
  if (table->count < table->capacity) {
    return;
  }
  size_t new_capacity = (table->capacity == 0)
                            ? kTacInterpInitialBindingCapacity
                            : table->capacity * kTacInterpGrowthFactor;
  struct TacFunctionEntry* next =
      (struct TacFunctionEntry*)realloc(table->entries, new_capacity * sizeof(*next));
  if (next == NULL) {
    tac_interp_error("memory allocation failed while growing TAC function table");
  }
  table->entries = next;
  table->capacity = new_capacity;
}

// Purpose: Find a function table entry by name.
// Inputs: table holds the function table; name is the identifier to find.
// Outputs: Returns the entry pointer or NULL if not found.
// Invariants/Assumptions: Name comparisons use slice equality.
static struct TacFunctionEntry* tac_function_table_find_name(struct TacFunctionTable* table,
                                                             struct Slice* name) {
  for (size_t i = 0; i < table->count; i++) {
    if (compare_slice_to_slice(table->entries[i].name, name)) {
      return &table->entries[i];
    }
  }
  return NULL;
}

// Purpose: Find a function table entry by address.
// Inputs: table holds the function table; address is the synthetic pointer value.
// Outputs: Returns the entry pointer or NULL if not found.
// Invariants/Assumptions: address values are unique per function.
static struct TacFunctionEntry* tac_function_table_find_address(struct TacFunctionTable* table,
                                                                int address) {
  for (size_t i = 0; i < table->count; i++) {
    if (table->entries[i].address == address) {
      return &table->entries[i];
    }
  }
  return NULL;
}

// Purpose: Register a function in the table and return its synthetic address.
// Inputs: table holds the function table; func is the function to register.
// Outputs: Returns the assigned synthetic address for func.
// Invariants/Assumptions: Each function name is registered at most once.
static int tac_function_table_register(struct TacFunctionTable* table,
                                       struct TopLevel* func) {
  if (func == NULL || func->name == NULL) {
    tac_interp_error("attempted to register a null function in TAC function table");
  }
  struct TacFunctionEntry* existing = tac_function_table_find_name(table, func->name);
  if (existing != NULL) {
    return existing->address;
  }
  if (table->next_address > INT_MAX - kTacInterpWordBytes) {
    tac_interp_error("function address space exhausted in TAC interpreter");
  }
  tac_function_table_reserve(table);
  struct TacFunctionEntry* entry = &table->entries[table->count++];
  entry->name = func->name;
  entry->func = func;
  entry->address = table->next_address;
  table->next_address += kTacInterpWordBytes;
  return entry->address;
}

// Purpose: Populate the function table from a TAC program.
// Inputs: table holds the function table; prog is the TAC program to scan.
// Outputs: Registers all top-level functions for pointer resolution.
// Invariants/Assumptions: prog is non-NULL and contains unique function names.
static void tac_function_table_populate(struct TacFunctionTable* table,
                                        struct TACProg* prog) {
  for (struct TopLevel* cur = prog->head; cur != NULL; cur = cur->next) {
    if (cur->type == FUNC) {
      (void)tac_function_table_register(table, cur);
    }
  }
}

// Purpose: Select the binding list that should store a variable.
// Inputs: interp is the interpreter state; frame is the current frame; name is the identifier.
// Outputs: Returns the bindings list to use for the identifier.
// Invariants/Assumptions: Globals are matched before locals.
static struct TacBindings* tac_select_bindings(struct TacInterpreter* interp,
                                               struct TacFrame* frame,
                                               struct Slice* name) {
  if (tac_bindings_find(&interp->globals, name) != NULL) {
    return &interp->globals;
  }
  return &frame->locals;
}

// Purpose: Read a variable value by name.
// Inputs: interp is the interpreter state; frame is the current frame; name is the variable name.
// Outputs: Returns the stored integer value.
// Invariants/Assumptions: Reading an unbound or uninitialized variable is an error.
static uint64_t tac_read_var(struct TacInterpreter* interp,
                             struct TacFrame* frame,
                             struct Slice* name) {
  struct TacBinding* binding = tac_bindings_find(&interp->globals, name);
  if (binding == NULL) {
    binding = tac_bindings_find(&frame->locals, name);
  }
  if (binding == NULL) {
    tac_interp_error("read from unknown variable %.*s", (int)name->len, name->start);
  }
  return tac_memory_load(&interp->memory, binding->address);
}

// Purpose: Write a variable value by name, creating storage if needed.
// Inputs: interp is the interpreter state; frame is the current frame; name is the variable name.
// Outputs: Stores the value in memory.
// Invariants/Assumptions: Unknown globals are treated as locals.
static void tac_write_var(struct TacInterpreter* interp,
                          struct TacFrame* frame,
                          struct Slice* name,
                          uint64_t value) {
  struct TacBindings* bindings = tac_select_bindings(interp, frame, name);
  struct TacBinding* binding = tac_bindings_get_or_add(bindings, &interp->memory, name);
  tac_memory_store(&interp->memory, binding->address, value);
}

// Purpose: Determine the base element size for a type (flattening arrays).
// Inputs: type is the declared variable type.
// Outputs: Returns the size in bytes of the innermost element type.
// Invariants/Assumptions: Only scalar/array types are expected here.
static size_t tac_base_element_size(struct Type* type) {
  struct Type* cur = type;
  while (cur != NULL && cur->type == ARRAY_TYPE) {
    cur = cur->type_data.array_type.element_type;
  }
  if (cur == NULL) {
    return 0;
  }
  return get_type_size((struct Type*)cur);
}

// Purpose: Map a static initializer kind to its byte size.
// Inputs: init_type is the static initializer kind.
// Outputs: Returns the size in bytes for one initializer entry.
// Invariants/Assumptions: ZERO_INIT is handled separately.
static size_t tac_static_init_size(enum StaticInitType init_type) {
  switch (init_type) {
    case CHAR_INIT:
    case UCHAR_INIT:
      return 1;
    case SHORT_INIT:
    case USHORT_INIT:
      return 2;
    case INT_INIT:
    case UINT_INIT:
      return 4;
    case LONG_INIT:
    case ULONG_INIT:
      return 8;
    case POINTER_INIT:
      return (size_t)kTacInterpWordBytes;
    case ZERO_INIT:
      return 0;
    default:
      return 0;
  }
}

// Purpose: Determine the slot count needed for an addressable type.
// Inputs: type is the TAC value type for the address-of source.
// Outputs: Returns the number of word slots to reserve.
// Invariants/Assumptions: Allocates enough space for the full type size.
static size_t tac_slots_for_type(struct Type* type) {
  if (type == NULL) {
    return kTacInterpSingleSlot;
  }
  size_t bytes = get_type_size((struct Type*)type);
  if (bytes == 0 || bytes == (size_t)-1) {
    tac_interp_error("invalid type size %zu for slot allocation", bytes);
  }
  return tac_slots_for_bytes(bytes);
}

// Purpose: Resolve the address associated with a variable name.
// Inputs: interp is the interpreter state; frame is the current frame; name is the variable name.
// Outputs: Returns the address for the variable, allocating if needed.
// Invariants/Assumptions: Unknown globals are treated as locals.
static int tac_address_of(struct TacInterpreter* interp,
                          struct TacFrame* frame,
                          struct Slice* name,
                          size_t slots) {
  struct TacBindings* bindings = tac_select_bindings(interp, frame, name);
  struct TacBinding* binding =
      tac_bindings_get_or_add_range(bindings, &interp->memory, name, slots);
  return binding->address;
}

// Purpose: Evaluate a TAC value to its integer representation.
// Inputs: interp is the interpreter state; frame is the current frame; val is the TAC value.
// Outputs: Returns the integer representation of the value.
// Invariants/Assumptions: Variable values must be initialized before use.
static uint64_t tac_eval_val(struct TacInterpreter* interp,
                             struct TacFrame* frame,
                             struct Val* val) {
  if (val == NULL) {
    tac_interp_error("attempted to evaluate a NULL TAC value");
  }
  switch (val->val_type) {
    case CONSTANT:
      return val->val.const_value;
    case VARIABLE:
      if (val->type != NULL && val->type->type == ARRAY_TYPE) {
        size_t slots = tac_slots_for_type(val->type);
        int addr = tac_address_of(interp, frame, val->val.var_name, slots);
        return (uint64_t)addr;
      }
      return tac_read_var(interp, frame, val->val.var_name);
    default:
      tac_interp_error("unknown TAC value type %d", (int)val->val_type);
      return 0;
  }
}

// Purpose: Assign a TAC value to a destination variable.
// Inputs: interp is the interpreter state; frame is the current frame; dst is the destination.
// Outputs: Writes the evaluated value into destination storage.
// Invariants/Assumptions: dst must be a VARIABLE value.
static void tac_assign_val(struct TacInterpreter* interp,
                           struct TacFrame* frame,
                           struct Val* dst,
                           uint64_t value) {
  if (dst == NULL || dst->val_type != VARIABLE) {
    tac_interp_error("assignment target is not a variable");
  }
  tac_write_var(interp, frame, dst->val.var_name, value);
}

// Purpose: Determine the result of a TAC conditional jump.
// Inputs: cond is the condition enum; left/right are the compared values.
// Outputs: Returns true if the condition is satisfied.
// Invariants/Assumptions: Signed comparisons use int64_t, unsigned use uint64_t.
static bool tac_condition_true(enum TACCondition cond, uint64_t left, uint64_t right) {
  switch (cond) {
    case CondE:
      return left == right;
    case CondNE:
      return left != right;
    case CondG:
      return (int64_t)left > (int64_t)right;
    case CondGE:
      return (int64_t)left >= (int64_t)right;
    case CondL:
      return (int64_t)left < (int64_t)right;
    case CondLE:
      return (int64_t)left <= (int64_t)right;
    case CondA:
      return left > right;
    case CondAE:
      return left >= right;
    case CondB:
      return left < right;
    case CondBE:
      return left <= right;
    default:
      tac_interp_error("unknown TAC condition %d", (int)cond);
      return false;
  }
}

// Purpose: Apply a unary operator to a value.
// Inputs: op is the unary operator; value is the operand.
// Outputs: Returns the result of the unary operation.
// Invariants/Assumptions: Boolean not returns 1 or 0.
static uint64_t tac_apply_unary(enum UnOp op, uint64_t value, struct Type* type) {
  uint64_t result = 0;
  switch (op) {
    case COMPLEMENT:
      result = ~value;
      break;
    case NEGATE:
      if (type != NULL && is_signed_type((struct Type*)type)) {
        result = (uint64_t)(-((int64_t)value));
      } else {
        result = (uint64_t)(0 - value);
      }
      break;
    case BOOL_NOT:
      result = (value == 0) ? 1u : 0u;
      break;
    case UNARY_PLUS:
      result = value;
      break;
    default:
      tac_interp_error("unsupported unary operator %d", (int)op);
      return 0;
  }
  return result;
}

// Purpose: Apply a binary operator to two values.
// Inputs: op is the binary operator; left/right are operands.
// Outputs: Returns the result of the binary operation.
// Invariants/Assumptions: Division or modulo by zero is an error.
static uint64_t tac_apply_binary(enum ALUOp op,
                                 uint64_t left,
                                 uint64_t right) {
  uint64_t result = 0;
  switch (op) {
    case ALU_ADD:
      result = left + right;
      break;
    case ALU_SUB:
      result = left - right;
      break;
    case ALU_SMUL:
      result = (uint64_t)(((int64_t)left) * ((int64_t)right));
      break;
    case ALU_UMUL:
      result = left * right;
      break;
    case ALU_SDIV:
      if ((int64_t)right == 0) {
        tac_interp_error("division by zero in TACBINARY");
      }
      result = (uint64_t)(((int64_t)left) / ((int64_t)right));
      break;
    case ALU_UDIV:
      if (right == 0) {
        tac_interp_error("division by zero in TACBINARY");
      }
      result = left / right;
      break;
    case ALU_SMOD:
      if ((int64_t)right == 0) {
        tac_interp_error("modulo by zero in TACBINARY");
      }
      result = (uint64_t)(((int64_t)left) % ((int64_t)right));
      break;
    case ALU_UMOD:
      if (right == 0) {
        tac_interp_error("modulo by zero in TACBINARY");
      }
      result = left % right;
      break;
    case ALU_AND:
      result = left & right;
      break;
    case ALU_OR:
      result = left | right;
      break;
    case ALU_XOR:
      result = left ^ right;
      break;
    case ALU_LSR: {
      uint64_t shift = right;
      result = left >> shift;
      break;
    }
    case ALU_LSL:
    case ALU_ASL: {
      uint64_t shift = right;
      result = left << shift;
      break;
    }
    case ALU_ASR: {
      uint64_t shift = right;
      result = (uint64_t)(((int64_t)left) >> shift);
      break;
    }
    case ALU_MOV:
      result = right;
      break;
    default:
      tac_interp_error("unsupported binary operator %d", (int)op);
      return 0;
  }
  return result;
}

// Purpose: Initialize label metadata for a function frame.
// Inputs: frame is the frame to populate; body is the function's instruction list.
// Outputs: Populates frame->labels for quick label lookup.
// Invariants/Assumptions: Duplicate labels are rejected.
static void tac_collect_labels(struct TacFrame* frame, struct TACInstr* body) {
  for (struct TACInstr* cur = body; cur != NULL; cur = cur->next) {
    if (cur->type != TACLABEL) {
      continue;
    }
    struct Slice* label = cur->instr.tac_label.label;
    for (size_t i = 0; i < frame->label_count; i++) {
      if (compare_slice_to_slice(frame->labels[i].label, label)) {
        tac_interp_error("duplicate label %.*s in TAC function",
                         (int)label->len, label->start);
      }
    }
    if (frame->label_count == frame->label_capacity) {
      size_t new_capacity = (frame->label_capacity == 0)
                                ? kTacInterpInitialLabelCapacity
                                : frame->label_capacity * kTacInterpGrowthFactor;
      struct TacLabelEntry* next =
          (struct TacLabelEntry*)realloc(frame->labels, new_capacity * sizeof(struct TacLabelEntry));
      if (next == NULL) {
        tac_interp_error("memory allocation failed while building label table");
      }
      frame->labels = next;
      frame->label_capacity = new_capacity;
    }
    frame->labels[frame->label_count].label = label;
    frame->labels[frame->label_count].instr = cur;
    frame->label_count++;
  }
}

// Purpose: Look up a label instruction in the current frame.
// Inputs: frame is the current frame; label is the label name.
// Outputs: Returns the instruction pointer for the label.
// Invariants/Assumptions: Labels are collected before execution.
static struct TACInstr* tac_find_label(struct TacFrame* frame, struct Slice* label) {
  for (size_t i = 0; i < frame->label_count; i++) {
    if (compare_slice_to_slice(frame->labels[i].label, label)) {
      return frame->labels[i].instr;
    }
  }
  tac_interp_error("unknown label %.*s in TAC jump", (int)label->len, label->start);
  return NULL;
}

// Purpose: Initialize a function frame before execution.
// Inputs: frame is the frame to initialize; interp is the interpreter state.
// Outputs: Sets up local bindings and label table for the function.
// Invariants/Assumptions: Caller supplies the function body.
static void tac_frame_init(struct TacFrame* frame, struct TacInterpreter* interp, struct TACInstr* body) {
  tac_bindings_init(&frame->locals);
  frame->labels = NULL;
  frame->label_count = 0;
  frame->label_capacity = 0;
  frame->cmp_valid = false;
  frame->cmp_left = 0;
  frame->cmp_right = 0;
  tac_collect_labels(frame, body);
  tac_preallocate_copy_offsets(frame, interp, body);
}

// Purpose: Release resources associated with a function frame.
// Inputs: frame is the frame to destroy.
// Outputs: Frees label and local binding storage.
// Invariants/Assumptions: Frame memory is heap-allocated.
static void tac_frame_destroy(struct TacFrame* frame) {
  tac_bindings_destroy(&frame->locals);
  free(frame->labels);
  frame->labels = NULL;
  frame->label_count = 0;
  frame->label_capacity = 0;
}

// Purpose: Handle built-in function calls that have no TAC definition.
// Inputs: call describes the call site; args are evaluated argument values.
// Outputs: Returns true if a builtin was handled and writes its result to out_result.
// Invariants/Assumptions: Builtins are limited to host-side I/O helpers like putchar.
static bool tac_try_builtin_call(struct TACCall* call,
                                 uint64_t* args,
                                 uint64_t* out_result) {
  if (compare_slice_to_pointer(call->func_name, kTacBuiltinPutcharName)) {
    if (call->num_args != kTacBuiltinPutcharArgCount) {
      tac_interp_error("putchar expects %zu argument, got %zu",
                       kTacBuiltinPutcharArgCount,
                       call->num_args);
    }
    int result = fputc((unsigned char)args[0], stdout);
    if (result == EOF) {
      tac_interp_error("putchar failed while writing output");
    }
    *out_result = (uint64_t)result;
    return true;
  }
  return false;
}

// Purpose: Execute a TAC function and return its result.
// Inputs: interp is the interpreter state; func is the function top-level node.
// Outputs: Returns the integer result produced by TACRETURN.
// Invariants/Assumptions: Function bodies include a TACRETURN on all paths.
static uint64_t tac_execute_function(struct TacInterpreter* interp,
                                     struct TopLevel* func,
                                     uint64_t* args,
                                     size_t num_args) {
  if (func == NULL || func->type != FUNC) {
    tac_interp_error("attempted to call a non-function top-level entry");
  }
  if (func->num_params != num_args) {
    tac_interp_error("argument count mismatch calling %.*s (expected %zu, got %zu)",
                     (int)func->name->len, func->name->start,
                     func->num_params, num_args);
  }

  struct TacFrame frame;
  tac_frame_init(&frame, interp, func->body);

  for (size_t i = 0; i < num_args; i++) {
    tac_write_var(interp, &frame, func->params[i], args[i]);
  }

  struct TACInstr* pc = func->body;
  while (pc != NULL) {
    switch (pc->type) {
      case TACRETURN: {
        uint64_t value = pc->instr.tac_return.dst
                             ? tac_eval_val(interp, &frame, pc->instr.tac_return.dst)
                             : 0;
        tac_frame_destroy(&frame);
        return value;
      }
      case TACUNARY: {
        uint64_t src = tac_eval_val(interp, &frame, pc->instr.tac_unary.src);
        struct Type* un_type =
            (pc->instr.tac_unary.dst != NULL) ? pc->instr.tac_unary.dst->type : NULL;
        if (un_type == NULL && pc->instr.tac_unary.src != NULL) {
          un_type = pc->instr.tac_unary.src->type;
        }
        uint64_t result = tac_apply_unary(pc->instr.tac_unary.op, src, un_type);
        tac_assign_val(interp, &frame, pc->instr.tac_unary.dst, result);
        break;
      }
      case TACBINARY: {
        uint64_t left = tac_eval_val(interp, &frame, pc->instr.tac_binary.src1);
        uint64_t right = tac_eval_val(interp, &frame, pc->instr.tac_binary.src2);
        uint64_t result = tac_apply_binary(pc->instr.tac_binary.alu_op, left, right);
        tac_assign_val(interp, &frame, pc->instr.tac_binary.dst, result);
        break;
      }
      case TACCOPY: {
        uint64_t value = tac_eval_val(interp, &frame, pc->instr.tac_copy.src);
        tac_assign_val(interp, &frame, pc->instr.tac_copy.dst, value);
        break;
      }
      case TACCMP: {
        frame.cmp_left = tac_eval_val(interp, &frame, pc->instr.tac_cmp.src1);
        frame.cmp_right = tac_eval_val(interp, &frame, pc->instr.tac_cmp.src2);
        frame.cmp_valid = true;
        break;
      }
      case TACCOND_JUMP: {
        if (!frame.cmp_valid) {
          tac_interp_error("conditional jump without prior compare");
        }
        if (tac_condition_true(pc->instr.tac_cond_jump.condition,
                               frame.cmp_left,
                               frame.cmp_right)) {
          pc = tac_find_label(&frame, pc->instr.tac_cond_jump.label);
          continue;
        }
        break;
      }
      case TACJUMP: {
        pc = tac_find_label(&frame, pc->instr.tac_jump.label);
        continue;
      }
      case TACLABEL:
        break;
      case TACCALL: {
        uint64_t* call_args = NULL;
        if (pc->instr.tac_call.num_args > 0) {
          call_args = (uint64_t*)malloc(sizeof(uint64_t) * pc->instr.tac_call.num_args);
          if (call_args == NULL) {
            tac_interp_error("memory allocation failed while preparing call arguments");
          }
          for (size_t i = 0; i < pc->instr.tac_call.num_args; i++) {
            call_args[i] = tac_eval_val(interp, &frame, &pc->instr.tac_call.args[i]);
          }
        }
        struct TopLevel* callee = NULL;
        for (struct TopLevel* cur = interp->prog->head; cur != NULL; cur = cur->next) {
          if (cur->type == FUNC && compare_slice_to_slice(cur->name, pc->instr.tac_call.func_name)) {
            callee = cur;
            break;
          }
        }
        if (callee == NULL) {
          uint64_t builtin_result = 0;
          if (tac_try_builtin_call(&pc->instr.tac_call, call_args, &builtin_result)) {
            free(call_args);
            if (pc->instr.tac_call.dst != NULL) {
              tac_assign_val(interp, &frame, pc->instr.tac_call.dst, builtin_result);
            }
            break;
          }
          tac_interp_error("call to unknown function %.*s",
                           (int)pc->instr.tac_call.func_name->len,
                           pc->instr.tac_call.func_name->start);
        }
        uint64_t result =
            tac_execute_function(interp, callee, call_args, pc->instr.tac_call.num_args);
        free(call_args);
        if (pc->instr.tac_call.dst != NULL) {
          tac_assign_val(interp, &frame, pc->instr.tac_call.dst, result);
        }
        break;
      }
      case TACCALL_INDIRECT: {
        uint64_t* call_args = NULL;
        if (pc->instr.tac_call_indirect.num_args > 0) {
          call_args =
              (uint64_t*)malloc(sizeof(uint64_t) * pc->instr.tac_call_indirect.num_args);
          if (call_args == NULL) {
            tac_interp_error("memory allocation failed while preparing indirect call arguments");
          }
          for (size_t i = 0; i < pc->instr.tac_call_indirect.num_args; i++) {
            call_args[i] = tac_eval_val(interp, &frame, &pc->instr.tac_call_indirect.args[i]);
          }
        }
        if (pc->instr.tac_call_indirect.func == NULL) {
          free(call_args);
          tac_interp_error("indirect call missing function operand");
        }
        uint64_t callee_addr_val = tac_eval_val(interp, &frame, pc->instr.tac_call_indirect.func);
        if (callee_addr_val == 0) {
          free(call_args);
          tac_interp_error("call through null function pointer");
        }
        if (callee_addr_val > (uint64_t)INT_MAX) {
          free(call_args);
          tac_interp_error("function pointer address out of range: %llu",
                           (unsigned long long)callee_addr_val);
        }
        struct TacFunctionEntry* callee_entry =
            tac_function_table_find_address(&interp->functions, (int)callee_addr_val);
        if (callee_entry == NULL || callee_entry->func == NULL) {
          free(call_args);
          tac_interp_error("call through unknown function pointer address %d",
                           (int)callee_addr_val);
        }
        uint64_t result = tac_execute_function(interp,
                                               callee_entry->func,
                                               call_args,
                                               pc->instr.tac_call_indirect.num_args);
        free(call_args);
        if (pc->instr.tac_call_indirect.dst != NULL) {
          tac_assign_val(interp, &frame, pc->instr.tac_call_indirect.dst, result);
        }
        break;
      }
      case TACGET_ADDRESS: {
        struct Val* src = pc->instr.tac_get_address.src;
        if (src == NULL || src->val_type != VARIABLE) {
          tac_interp_error("get-address requires a variable source");
        }
        if (src->type != NULL && src->type->type == FUN_TYPE) {
          struct TacFunctionEntry* entry =
              tac_function_table_find_name(&interp->functions, src->val.var_name);
          if (entry == NULL) {
            tac_interp_error("unknown function in get-address %.*s",
                             (int)src->val.var_name->len,
                             src->val.var_name->start);
          }
          tac_assign_val(interp, &frame, pc->instr.tac_get_address.dst,
                         (uint64_t)entry->address);
          break;
        }
        size_t slots = tac_slots_for_type(src->type);
        int addr = tac_address_of(interp, &frame, src->val.var_name, slots);
        tac_assign_val(interp, &frame, pc->instr.tac_get_address.dst, (uint64_t)addr);
        break;
      }
      case TACLOAD: {
        int addr = (int)tac_eval_val(interp, &frame, pc->instr.tac_load.src_ptr);
        uint64_t value = tac_memory_load(&interp->memory, addr);
        tac_assign_val(interp, &frame, pc->instr.tac_load.dst, value);
        break;
      }
      case TACSTORE: {
        int addr = (int)tac_eval_val(interp, &frame, pc->instr.tac_store.dst_ptr);
        uint64_t value = tac_eval_val(interp, &frame, pc->instr.tac_store.src);
        tac_memory_store(&interp->memory, addr, value);
        break;
      }
      case TACCOPY_TO_OFFSET: {
        struct Slice* dst = pc->instr.tac_copy_to_offset.dst;
        if (dst == NULL) {
          tac_interp_error("copy-to-offset missing destination");
        }
        struct TacBinding* binding = tac_bindings_find(&interp->globals, dst);
        if (binding == NULL) {
          binding = tac_bindings_find(&frame.locals, dst);
        }
        if (binding == NULL) {
          tac_interp_error("copy-to-offset unknown base %.*s",
                           (int)dst->len, dst->start);
        }
        if (pc->instr.tac_copy_to_offset.offset < 0) {
          tac_interp_error("copy-to-offset negative offset %d",
                           pc->instr.tac_copy_to_offset.offset);
        }
        uint64_t value = tac_eval_val(interp, &frame, pc->instr.tac_copy_to_offset.src);
        int addr = binding->address + pc->instr.tac_copy_to_offset.offset;
        tac_memory_store(&interp->memory, addr, value);
        break;
      }
      case TACCOPY_FROM_OFFSET: {
        struct Slice* src = pc->instr.tac_copy_from_offset.src;
        struct Val* dst = pc->instr.tac_copy_from_offset.dst;
        if (src == NULL || dst == NULL) {
          tac_interp_error("copy-from-offset missing source or destination");
        }
        struct TacBinding* binding = tac_bindings_find(&interp->globals, src);
        if (binding == NULL) {
          binding = tac_bindings_find(&frame.locals, src);
        }
        if (binding == NULL) {
          tac_interp_error("copy-from-offset unknown base %.*s",
                           (int)src->len, src->start);
        }
        if (pc->instr.tac_copy_from_offset.offset < 0) {
          tac_interp_error("copy-from-offset negative offset %d",
                           pc->instr.tac_copy_from_offset.offset);
        }
        int addr = binding->address + pc->instr.tac_copy_from_offset.offset;
        uint64_t value = tac_memory_load(&interp->memory, addr);
        tac_assign_val(interp, &frame, dst, value);
        break;
      }
      case TACBOUNDARY:
        // No operation needed for boundary markers in the interpreter.
        break;
      case TACTRUNC: {
        uint64_t value = tac_eval_val(interp, &frame, pc->instr.tac_trunc.src);
        size_t target_bits = pc->instr.tac_trunc.target_size * 8;
        // Truncate by masking, guarding against shifts of 64 bits.
        uint64_t mask = target_bits >= 64 ? ~0ull : ((1ull << target_bits) - 1ull);
        tac_assign_val(interp, &frame, pc->instr.tac_trunc.dst, value & mask);
        break;
      }
      case TACEXTEND: {
        uint64_t value = tac_eval_val(interp, &frame, pc->instr.tac_extend.src);
        size_t src_bits = pc->instr.tac_extend.src_size * 8;
        // Sign-extend when the source width is narrower than 64 bits.
        if (src_bits < 64) {
          uint64_t sign_bit = 1ull << (src_bits - 1);
          if ((value & sign_bit) != 0) {
            uint64_t extend_mask = ~0ull << src_bits;
            value |= extend_mask;
          }
        }
        tac_assign_val(interp, &frame, pc->instr.tac_extend.dst, value);
        break;
      }
      default:
        tac_interp_error("unsupported TAC instruction %d in interpreter", (int)pc->type);
    }
    pc = pc->next;
  }

  tac_frame_destroy(&frame);
  tac_interp_error("function %.*s terminated without TACRETURN",
                   (int)func->name->len, func->name->start);
  return 0;
}

// Purpose: Initialize global/static variables from TAC top-level entries.
// Inputs: interp is the interpreter state; prog is the TAC program.
// Outputs: Allocates storage and sets initial values in global memory.
// Invariants/Assumptions: Static init lists are stored in consecutive word slots.
static void tac_init_globals(struct TacInterpreter* interp, struct TACProg* prog) {
  struct TopLevel* cur = (prog->statics != NULL) ? prog->statics : prog->head;
  for (; cur != NULL; cur = cur->next) {
    if (cur->type != STATIC_VAR && cur->type != STATIC_CONST) {
      continue;
    }
    size_t slots = tac_slots_for_type(cur->var_type);
    (void)tac_bindings_get_or_add_range(&interp->globals, &interp->memory, cur->name, slots);
  }

  for (cur = (prog->statics != NULL) ? prog->statics : prog->head; cur != NULL; cur = cur->next) {
    if (cur->type != STATIC_VAR && cur->type != STATIC_CONST) {
      continue;
    }
    struct TacBinding* binding = tac_bindings_find(&interp->globals, cur->name);
    if (binding == NULL) {
      tac_interp_error("missing static binding for %.*s",
                       (int)cur->name->len, cur->name->start);
    }
    int base_addr = binding->address;
    size_t total_bytes = get_type_size(cur->var_type);
    if (total_bytes == 0) {
      tac_interp_error("zero-sized static allocation for %.*s",
                       (int)cur->name->len, cur->name->start);
    }
    size_t elem_size = tac_base_element_size(cur->var_type);
    if (elem_size == 0) {
      tac_interp_error("unknown static element size for %.*s",
                       (int)cur->name->len, cur->name->start);
    }

    // Zero-fill the full allocation first to handle implicit zero init.
    for (size_t offset = 0; offset < total_bytes; offset += elem_size) {
      tac_memory_store(&interp->memory, base_addr + (int)offset, 0);
    }

    struct InitList* init = cur->init_values;
    size_t offset = 0;
    while (init != NULL) {
      struct StaticInit* init_value = init->value;
      if (init_value == NULL) {
        tac_interp_error("null static initializer for %.*s",
                         (int)cur->name->len, cur->name->start);
      }

      if (init_value->int_type == ZERO_INIT) {
        size_t zero_bytes = (size_t)init_value->value.num;
        for (size_t z = 0; z < zero_bytes; z += elem_size) {
          tac_memory_store(&interp->memory, base_addr + (int)(offset + z), 0);
        }
        offset += zero_bytes;
        init = init->next;
        continue;
      }

      if (init_value->int_type == STRING_INIT) {
        struct Slice* str = init_value->value.string;
        if (str == NULL) {
          tac_interp_error("null string initializer for %.*s",
                           (int)cur->name->len, cur->name->start);
        }
        for (size_t i = 0; i < str->len; i++) {
          unsigned char byte = (unsigned char)str->start[i];
          tac_memory_store(&interp->memory, base_addr + (int)(offset + i), (uint64_t)byte);
        }
        offset += str->len;
        init = init->next;
        continue;
      }

      if (init_value->int_type == POINTER_INIT) {
        struct Slice* target = init_value->value.pointer;
        if (target == NULL) {
          tac_interp_error("null pointer initializer for %.*s",
                           (int)cur->name->len, cur->name->start);
        }
        struct TacBinding* target_binding = tac_bindings_find(&interp->globals, target);
        if (target_binding == NULL) {
          tac_interp_error("unknown static pointer target %.*s",
                           (int)target->len, target->start);
        }
        tac_memory_store(&interp->memory, base_addr + (int)offset,
                         (uint64_t)target_binding->address);
        offset += (size_t)kTacInterpWordBytes;
        init = init->next;
        continue;
      }

      size_t init_size = tac_static_init_size(init_value->int_type);
      if (init_size == 0) {
        tac_interp_error("unsupported static init type %d for %.*s",
                         (int)init_value->int_type,
                         (int)cur->name->len, cur->name->start);
      }
      tac_memory_store(&interp->memory, base_addr + (int)offset, init_value->value.num);
      offset += init_size;
      init = init->next;
    }
  }
}

// Purpose: Find the "main" function in a TAC program.
// Inputs: prog is the TAC program; name is the function name to locate.
// Outputs: Returns the matching function node or NULL if missing.
// Invariants/Assumptions: Function names are unique at top level.
static struct TopLevel* tac_find_function(struct TACProg* prog, struct Slice* name) {
  for (struct TopLevel* cur = prog->head; cur != NULL; cur = cur->next) {
    if (cur->type == FUNC && compare_slice_to_slice(cur->name, name)) {
      return cur;
    }
  }
  return NULL;
}

// Purpose: Report missing entry point with available function names.
// Inputs: prog is the TAC program searched for main.
// Outputs: Writes diagnostics to stderr and terminates execution.
// Invariants/Assumptions: Function names are stored as slices in the TAC program.
static void tac_report_missing_main(struct TACProg* prog) {
  fprintf(stderr, "TAC Interpreter Error: no main function found in TAC program\n");
  fprintf(stderr, "TAC Interpreter Error: expected main length %zu\n", kTacInterpMainNameLen);
  if (prog->head != NULL) {
    fprintf(stderr, "TAC Interpreter Error: head=%.*s(len=%zu)\n",
            (int)prog->head->name->len,
            prog->head->name->start,
            prog->head->name->len);
  }
  if (prog->tail != NULL) {
    fprintf(stderr, "TAC Interpreter Error: tail=%.*s(len=%zu) type=%d\n",
            (int)prog->tail->name->len,
            prog->tail->name->start,
            prog->tail->name->len,
            (int)prog->tail->type);
  }
  for (struct TopLevel* cur = prog->head; cur != NULL; cur = cur->next) {
    fprintf(stderr, "TAC Interpreter Error: node type=%d name=%.*s(len=%zu)\n",
            (int)cur->type,
            (int)cur->name->len,
            cur->name->start,
            cur->name->len);
  }
  fprintf(stderr, "TAC Interpreter Error: available functions:");
  bool found = false;
  for (struct TopLevel* cur = prog->head; cur != NULL; cur = cur->next) {
    if (cur->type == FUNC) {
      fprintf(stderr, " %.*s(len=%zu)", (int)cur->name->len, cur->name->start, cur->name->len);
      found = true;
    }
  }
  if (!found) {
    fprintf(stderr, " <none>");
  }
  fprintf(stderr, "\n");
  exit(EXIT_FAILURE);
}

// Purpose: Interpret a TAC program and return the result of main().
// Inputs: prog is the TAC program to execute.
// Outputs: Returns the integer result of the main function.
// Invariants/Assumptions: main takes no parameters in this interpreter.
int tac_interpret_prog(struct TACProg* prog) {
  if (prog == NULL) {
    tac_interp_error("cannot interpret a NULL TAC program");
  }

  struct TacInterpreter interp;
  interp.prog = prog;
  tac_memory_init(&interp.memory);
  tac_bindings_init(&interp.globals);
  tac_function_table_init(&interp.functions);
  tac_function_table_populate(&interp.functions, prog);

  struct Slice main_name = { .start = "main", .len = kTacInterpMainNameLen };
  struct TopLevel* main_func = tac_find_function(prog, &main_name);
  if (main_func == NULL) {
    if (prog->tail != NULL &&
        prog->tail->type == FUNC &&
        compare_slice_to_slice(prog->tail->name, &main_name)) {
      main_func = prog->tail;
    } else {
      tac_report_missing_main(prog);
    }
  }
  if (main_func->num_params != 0) {
    tac_interp_error("main function expects %zu parameters; interpreter requires 0",
                     main_func->num_params);
  }

  tac_init_globals(&interp, prog);

  uint64_t result = tac_execute_function(&interp, main_func, NULL, 0);
  tac_function_table_destroy(&interp.functions);
  tac_bindings_destroy(&interp.globals);
  tac_memory_destroy(&interp.memory);
  return (int)result;
}
