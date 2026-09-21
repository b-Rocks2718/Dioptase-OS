#ifndef IDENTIFIER_MAP_H
#define IDENTIFIER_MAP_H

#include "../crt/stddef.h"
#include "../crt/stdint.h"
#include "../crt/stdbool.h"

#include "slice.h"
#include "types.h"

struct IdentMap;

// Provide scoped identifier lookup for identifier resolution.
// Supports stack-based scoping and hash map operations.
// Maps store pointers to slices; memory is owned elsewhere.

// Stack of identifier maps, one per scope.
// Used by identifier resolution to search from inner to outer scope.
struct IdentStack {
  struct IdentMap** maps;
  size_t size;
  size_t capacity;
};

// Entry in an identifier hash map.
// Stored in IdentMap buckets for lookups.
struct IdentMapEntry{
  struct Slice* key;
  struct Slice* entry_name;
  bool has_linkage; // used by var map
  bool is_const; // used by var map
  unsigned value; // used by var map for enum constant value
  enum TypeType type; // used by type map
  struct IdentMapEntry* next;
};

// Hash map of identifiers for a single scope.
// Supports scoped identifier lookup.
struct IdentMap{
	size_t size;
  struct IdentMapEntry** arr;
};

// Allocate a scoped identifier stack with the requested scope capacity.
// Returns an allocated IdentStack with no scopes.
// Caller must destroy the stack when done.
struct IdentStack* create_ident_stack(size_t initial_capacity);

// Push the initial empty scope onto an identifier stack.
// Returns an IdentStack with a single scope map.
struct IdentStack* init_scope(void);

// Push a new empty scope onto the stack.
// Adds a new IdentMap on top of the stack.
void enter_scope(struct IdentStack* stack);

// Pop and destroy the top scope from the stack.
// Removes the current IdentMap and returns it.
struct IdentMap* exit_scope(struct IdentStack* stack);

// Look up an identifier across scopes.
// Returns the entry and sets from_current_scope accordingly.
struct IdentMapEntry* ident_stack_get(struct IdentStack* stack, struct Slice* key, bool* from_current_scope);

// Check if an identifier exists in the current scope.
// Returns true if found in the top scope.
bool ident_stack_in_current_scope(struct IdentStack* stack, struct Slice* key);

// Insert an identifier mapping into the current scope.
void ident_stack_insert(struct IdentStack* stack, struct Slice* key, 
    struct Slice* entry_name, bool has_linkage, enum TypeType type, bool is_const, unsigned value);

// Destroy all scopes in the stack.
void destroy_ident_stack(struct IdentStack* stack);

// Look up an identifier in a single scope map.
// Returns the entry or NULL if missing.
struct IdentMapEntry* ident_map_get(struct IdentMap* hmap, struct Slice* key);

// Insert or update an identifier mapping in a scope map.
void ident_map_insert(struct IdentMap* hmap, struct Slice* key, 
    struct Slice* entry_name, bool has_linkage, enum TypeType type, bool is_const, unsigned value);

// Allocate an identifier map with the requested bucket count.
// Returns an allocated IdentMap.
// Caller must destroy the map when done.
struct IdentMap* create_ident_map(size_t size);

// Destroy an identifier map and its entries.
void destroy_ident_map(struct IdentMap* hmap);

#endif // IDENTIFIER_MAP_H
