#include "../crt/stdlib.h"
#include "../crt/print.h"

#include "identifier_map.h"
#include "slice.h"

// Purpose: Implement scoped identifier maps for name resolution.
// Inputs: Keys and values are slices; has_linkage tracks external linkage.
// Outputs: Provides scope stack and hash map operations.
// Invariants/Assumptions: Maps store pointers to slices; entries are heap-allocated.

// Purpose: Default bucket count for per-scope identifier maps.
// Inputs: Used by enter_scope to size new maps.
// Outputs: Controls hash table size.
// Invariants/Assumptions: Chosen to balance memory and lookup speed.
#define IDENT_MAP_BUCKETS 1024
// Purpose: Default initial capacity for the scope stack.
// Inputs: Used by init_scope to size the stack array.
// Outputs: Controls how many scopes fit before resizing.
// Invariants/Assumptions: Stack grows dynamically when exceeded.
#define INITIAL_STACK_CAPACITY 8

// Purpose: Initialize a new identifier stack with one scope.
// Inputs: None.
// Outputs: Returns an IdentStack ready for lookups.
// Invariants/Assumptions: Caller must destroy the stack when done.
struct IdentStack* init_scope(void) {
  struct IdentStack* stack = create_ident_stack(INITIAL_STACK_CAPACITY);
  enter_scope(stack);
  return stack;
}

// Purpose: Allocate an identifier stack with a given capacity.
// Inputs: initial_capacity sets the initial number of scope slots.
// Outputs: Returns a heap-allocated IdentStack with no scopes.
// Invariants/Assumptions: Caller must push scopes before lookup/insert.
struct IdentStack* create_ident_stack(size_t initial_capacity){
  struct IdentMap** maps = malloc(initial_capacity * sizeof(struct IdentMap*));
  struct IdentStack* stack = malloc(sizeof(struct IdentStack));

  stack->maps = maps;
  stack->size = 0;
  stack->capacity = initial_capacity;

  return stack;
}

// Purpose: Push a map onto the scope stack, resizing if needed.
// Inputs: stack is the identifier stack; map is the scope to add.
// Outputs: Adds map to the top of the stack.
// Invariants/Assumptions: Stack grows by doubling capacity.
void ident_stack_push(struct IdentStack* stack, struct IdentMap* map){
  if (stack->size >= stack->capacity){
    size_t new_capacity = stack->capacity * 2;
    struct IdentMap** new_maps = malloc(new_capacity * sizeof(struct IdentMap*));
    for (int i = 0; i < stack->size; ++i){
      new_maps[i] = stack->maps[i];
    }
    free(stack->maps);
    stack->maps = new_maps;
    stack->capacity = new_capacity;
  }
  stack->maps[stack->size] = map;
  stack->size += 1;
}

// Purpose: Pop the top scope map from the stack.
// Inputs: stack is the identifier stack.
// Outputs: Returns the popped IdentMap or NULL if the stack is empty.
// Invariants/Assumptions: Caller is responsible for destroying the map.
struct IdentMap* ident_stack_pop(struct IdentStack* stack){
  if (stack->size == 0){
    return NULL;
  } else {
    stack->size -= 1;
    return stack->maps[stack->size];
  }
}

// Purpose: Peek at the current scope map without removing it.
// Inputs: stack is the identifier stack.
// Outputs: Returns the top IdentMap or NULL if the stack is empty.
// Invariants/Assumptions: Returned map remains owned by the stack.
struct IdentMap* ident_stack_peek(struct IdentStack* stack){
  if (stack->size == 0){
    return NULL;
  } else {
    return stack->maps[stack->size - 1];
  }
}

// Purpose: Look up an identifier across all scopes.
// Inputs: stack is the identifier stack; key is the lookup name.
// Outputs: Returns the entry and sets from_current_scope.
// Invariants/Assumptions: Searches from innermost scope outward.
struct IdentMapEntry* ident_stack_get(struct IdentStack* stack, struct Slice* key, bool* from_current_scope){
  for (int i = stack->size - 1; i >= 0; --i){
    struct IdentMapEntry* entry = ident_map_get(stack->maps[i], key);
    if (entry != NULL){
      if (from_current_scope != NULL){
        *from_current_scope = (i == stack->size - 1);
      }
      return entry;
    }
  }
  return NULL;
}

// Purpose: Check if a name is declared in the current scope.
// Inputs: stack is the identifier stack; key is the lookup name.
// Outputs: Returns true if found in the top scope.
// Invariants/Assumptions: Prints an error if no scope exists.
bool ident_stack_in_current_scope(struct IdentStack* stack, struct Slice* key){
  struct IdentMap* current_map = ident_stack_peek(stack);
  if (current_map != NULL){
    struct IdentMapEntry* entry = ident_map_get(current_map, key);
    return (entry != NULL);
  } else {
    // error: no map to check
    fdputs(STDERR, "Identifier Map Error: No map in stack to check\n");
    return false;
  }
}

// Purpose: Insert a name mapping into the current scope.
// Inputs: stack is the identifier stack; key/entry_name are slices.
// Outputs: Updates the current scope map.
// Invariants/Assumptions: Current scope exists; errors are printed otherwise.
void ident_stack_insert(struct IdentStack* stack, struct Slice* key, 
    struct Slice* entry_name, bool has_linkage, enum TypeType type, bool is_const, unsigned value){
  struct IdentMap* current_map = ident_stack_peek(stack);
  if (current_map != NULL){
    ident_map_insert(current_map, key, entry_name, has_linkage, type, is_const, value);
  } else {
    // error: no map to insert into
    fdputs(STDERR, "Identifier Map Error: No map in stack to insert into\n");
  }
}

// Purpose: Push a new empty scope onto the stack.
// Inputs: stack is the identifier stack.
// Outputs: Adds a new IdentMap with default bucket count.
// Invariants/Assumptions: Uses IDENT_MAP_BUCKETS for sizing.
void enter_scope(struct IdentStack* stack){
  struct IdentMap* new_map = create_ident_map(IDENT_MAP_BUCKETS);
  ident_stack_push(stack, new_map);
}

// Purpose: Pop and destroy the current scope.
// Inputs: stack is the identifier stack.
// Outputs: Returns the top IdentMap and removes it from the stack.
// Invariants/Assumptions: Prints an error if the stack is empty.
struct IdentMap* exit_scope(struct IdentStack* stack){
  struct IdentMap* old_map = ident_stack_pop(stack);
  if (old_map != NULL){
    return old_map;
  } else {
    // error: no map to pop
    fdputs(STDERR, "Identifier Map Error: No map in stack to pop\n");
    exit(1);
  }
}

// Purpose: Destroy the entire identifier stack and all scopes.
// Inputs: stack is the identifier stack.
// Outputs: Frees all maps and the stack container.
// Invariants/Assumptions: Does not free slices referenced by entries.
void destroy_ident_stack(struct IdentStack* stack){
  for (int i = 0; i < stack->size; ++i){
    destroy_ident_map(stack->maps[i]);
  }
  free(stack->maps);
  free(stack);
}

// Purpose: Allocate a new identifier map with a given bucket count.
// Inputs: num_buckets is the number of hash buckets.
// Outputs: Returns a heap-allocated IdentMap.
// Invariants/Assumptions: Caller must destroy the map when done.
struct IdentMap* create_ident_map(size_t num_buckets){
  struct IdentMapEntry** arr = malloc(num_buckets * sizeof(struct IdentMapEntry*));
  struct IdentMap* hmap = malloc(sizeof(struct IdentMap));

  for (int i = 0; i < num_buckets; ++i){
    arr[i] = NULL;
  }

  hmap->size = num_buckets;
  hmap->arr = arr;

  return hmap;
}

// Purpose: Allocate a new identifier map entry.
// Inputs: key is the original name; entry_name is the resolved name.
// Outputs: Returns a heap-allocated IdentMapEntry.
// Invariants/Assumptions: key/entry_name pointers remain valid for entry lifetime.
struct IdentMapEntry* create_ident_map_entry(struct Slice* key, 
    struct Slice* entry_name, bool has_linkage, enum TypeType type, bool is_const, unsigned value){
  struct IdentMapEntry* entry = malloc(sizeof(struct IdentMapEntry));

  entry->key = key;
  entry->entry_name = entry_name;
  entry->has_linkage = has_linkage;
  entry->type = type;
  entry->is_const = is_const;
  entry->value = value;
  entry->next = NULL;

  return entry;
}

// Purpose: Insert or update a mapping within a bucket chain.
// Inputs: entry is the chain head; key/entry_name/has_linkage are the mapping.
// Outputs: Updates or appends to the chain.
// Invariants/Assumptions: Uses recursion for traversal.
void ident_map_entry_insert(struct IdentMapEntry* entry, struct Slice* key, 
    struct Slice* entry_name, bool has_linkage, enum TypeType type, bool is_const, unsigned value){
  if (compare_slice_to_slice(entry->key, key)){
    entry->entry_name = entry_name;
    entry->has_linkage = has_linkage;
    entry->type = type;
    entry->is_const = is_const;
    entry->value = value;
  } else if (entry->next == NULL){
    entry->next = create_ident_map_entry(key, entry_name, has_linkage, type, is_const, value);
  } else {
    ident_map_entry_insert(entry->next, key, entry_name, has_linkage, type, is_const, value);
  }
}

// Purpose: Insert or update a mapping in the identifier map.
// Inputs: hmap is the map; key/entry_name/has_linkage are the mapping.
// Outputs: Updates the map in place.
// Invariants/Assumptions: hash_slice produces stable bucket indices.
void ident_map_insert(struct IdentMap* hmap, struct Slice* key, 
    struct Slice* entry_name, bool has_linkage, enum TypeType type, bool is_const, unsigned value){
  size_t hash = hash_slice(key) % hmap->size;
  
  if ((hmap->arr[hash]) == NULL){
    hmap->arr[hash] = create_ident_map_entry(key, entry_name, has_linkage, type, is_const, value);
  } else {
    ident_map_entry_insert(hmap->arr[hash], key, entry_name, has_linkage, type, is_const, value);
  }
}

// Purpose: Look up an identifier within a bucket chain.
// Inputs: entry is the chain head; key is the lookup name.
// Outputs: Returns the entry or NULL if missing.
// Invariants/Assumptions: compare_slice_to_slice defines key equality.
struct IdentMapEntry* ident_map_entry_get(struct IdentMapEntry* entry, struct Slice* key){
  if (compare_slice_to_slice(entry->key, key)){
    return entry;
  } else if (entry->next == NULL){
    return NULL;
  } else {
    return ident_map_entry_get(entry->next, key);
  }
}

// Purpose: Look up an identifier in the map.
// Inputs: hmap is the map; key is the lookup name.
// Outputs: Returns the entry or NULL if missing.
// Invariants/Assumptions: hash_slice is consistent with insertions.
struct IdentMapEntry* ident_map_get(struct IdentMap* hmap, struct Slice* key){
  size_t hash = hash_slice(key) % hmap->size;

  if (hmap->arr[hash] == NULL){
    return NULL;
  } else {
    return ident_map_entry_get(hmap->arr[hash], key);
  }
}

// Purpose: Free a bucket chain of identifier entries.
// Inputs: entry is the chain head.
// Outputs: Frees each IdentMapEntry node.
// Invariants/Assumptions: Does not free slices referenced by entries.
void destroy_ident_map_entry(struct IdentMapEntry* entry){
  if (entry->next != NULL) destroy_ident_map_entry(entry->next);
  free(entry);
}

// Purpose: Destroy an identifier map and its entries.
// Inputs: hmap is the map to destroy.
// Outputs: Frees bucket storage and entry nodes.
// Invariants/Assumptions: Caller must not use hmap after destruction.
void destroy_ident_map(struct IdentMap* hmap){
  for (int i = 0; i < hmap->size; ++i){
    if (hmap->arr[i] != NULL) destroy_ident_map_entry(hmap->arr[i]);
  }
  free(hmap->arr);
  free(hmap);
}
