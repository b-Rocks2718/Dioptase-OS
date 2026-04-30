#ifndef IDENTIFIER_MAP_H
#define IDENTIFIER_MAP_H

#include "../crt/stddef.h"
#include "../crt/stdint.h"
#include "../crt/stdbool.h"

#include "slice.h"
#include "types.h"

struct IdentMap;

// Purpose: Provide scoped identifier lookup for identifier resolution.
// Inputs: Keys are identifier slices; values are unique-name slices.
// Outputs: Supports stack-based scoping and hash map operations.
// Invariants/Assumptions: Maps store pointers to slices; memory is owned elsewhere.

// Purpose: Stack of identifier maps, one per scope.
// Inputs: maps holds the per-scope maps; size/capacity track the stack.
// Outputs: Used by identifier resolution to search from inner to outer scope.
// Invariants/Assumptions: maps[0..size-1] are valid IdentMap pointers.
struct IdentStack {
  struct IdentMap** maps;
  size_t size;
  size_t capacity;
};

// Purpose: Entry in an identifier hash map.
// Inputs: key is the source identifier; entry_name is the resolved name.
// Outputs: Stored in IdentMap buckets for lookups.
// Invariants/Assumptions: has_linkage indicates external linkage visibility.
struct IdentMapEntry{
  struct Slice* key;
  struct Slice* entry_name;
  bool has_linkage; // used by var map
  bool is_const; // used by var map
  unsigned value; // used by var map for enum constant value
  enum TypeType type; // used by type map
  struct IdentMapEntry* next;
};

// Purpose: Hash map of identifiers for a single scope.
// Inputs: size is the bucket count; arr stores bucket chains.
// Outputs: Supports scoped identifier lookup.
// Invariants/Assumptions: size is non-zero; arr length equals size.
struct IdentMap{
	size_t size;
  struct IdentMapEntry** arr;
};

// Purpose: Create an identifier stack with a given initial capacity.
// Inputs: initial_capacity is the starting number of scope slots.
// Outputs: Returns an allocated IdentStack with no scopes.
// Invariants/Assumptions: Caller must destroy the stack when done.
struct IdentStack* create_ident_stack(size_t initial_capacity);

// Purpose: Initialize a stack with one empty scope.
// Inputs: None.
// Outputs: Returns an IdentStack with a single scope map.
// Invariants/Assumptions: Uses default bucket count for maps.
struct IdentStack* init_scope(void);

// Purpose: Push a new empty scope onto the stack.
// Inputs: stack is the identifier stack.
// Outputs: Adds a new IdentMap on top of the stack.
// Invariants/Assumptions: Stack grows dynamically as needed.
void enter_scope(struct IdentStack* stack);

// Purpose: Pop and destroy the top scope from the stack.
// Inputs: stack is the identifier stack.
// Outputs: Removes the current IdentMap and returns it.
// Invariants/Assumptions: No lookups should occur in the popped scope afterward.
struct IdentMap* exit_scope(struct IdentStack* stack);

// Purpose: Look up an identifier across scopes.
// Inputs: stack is the identifier stack; key is the lookup name.
// Outputs: Returns the entry and sets from_current_scope accordingly.
// Invariants/Assumptions: Searches from innermost to outermost scope.
struct IdentMapEntry* ident_stack_get(struct IdentStack* stack, struct Slice* key, bool* from_current_scope);

// Purpose: Check if an identifier exists in the current scope.
// Inputs: stack is the identifier stack; key is the lookup name.
// Outputs: Returns true if found in the top scope.
// Invariants/Assumptions: Returns false and prints if no scope exists.
bool ident_stack_in_current_scope(struct IdentStack* stack, struct Slice* key);

// Purpose: Insert an identifier mapping into the current scope.
// Inputs: stack is the identifier stack; key and entry_name are slices.
// Outputs: Updates the current scope map.
// Invariants/Assumptions: Caller has already ensured scope exists.
void ident_stack_insert(struct IdentStack* stack, struct Slice* key, 
    struct Slice* entry_name, bool has_linkage, enum TypeType type, bool is_const, unsigned value);

// Purpose: Destroy all scopes in the stack.
// Inputs: stack is the identifier stack.
// Outputs: Frees all maps and the stack container.
// Invariants/Assumptions: Does not free slices referenced by entries.
void destroy_ident_stack(struct IdentStack* stack);

// Purpose: Look up an identifier in a single scope map.
// Inputs: hmap is the map; key is the lookup name.
// Outputs: Returns the entry or NULL if missing.
// Invariants/Assumptions: hash_slice is consistent with inserts.
struct IdentMapEntry* ident_map_get(struct IdentMap* hmap, struct Slice* key);

// Purpose: Insert or update an identifier mapping in a scope map.
// Inputs: hmap is the map; key/entry_name are slices; has_linkage marks linkage.
// Outputs: Updates the map in place.
// Invariants/Assumptions: Map stores pointer references to slices.
void ident_map_insert(struct IdentMap* hmap, struct Slice* key, 
    struct Slice* entry_name, bool has_linkage, enum TypeType type, bool is_const, unsigned value);

// Purpose: Create a new identifier map with a given bucket count.
// Inputs: size is the number of buckets.
// Outputs: Returns an allocated IdentMap.
// Invariants/Assumptions: Caller must destroy the map when done.
struct IdentMap* create_ident_map(size_t size);

// Purpose: Destroy an identifier map and its entries.
// Inputs: hmap is the map to destroy.
// Outputs: Frees bucket storage and entries.
// Invariants/Assumptions: Does not free slices referenced by entries.
void destroy_ident_map(struct IdentMap* hmap);

#endif // IDENTIFIER_MAP_H
