#include "../crt/stdlib.h"
#include "../crt/print.h"

#include "identifier_map.h"
#include "slice.h"

// Implement scoped identifier maps for name resolution.
// Provides scope stack and hash map operations.

// Default bucket count for per-scope identifier maps.
// Controls hash table size.
#define IDENT_MAP_BUCKETS 1024
// Default initial capacity for the scope stack.
// Controls how many scopes fit before resizing.
#define INITIAL_STACK_CAPACITY 8

// Allocate a scope stack whose initial top scope is empty.
// Returns an IdentStack ready for lookups.
// Caller must destroy the stack when done.
struct IdentStack* init_scope(void) {
  struct IdentStack* stack = create_ident_stack(INITIAL_STACK_CAPACITY);
  enter_scope(stack);
  return stack;
}

// Allocate an identifier stack with a given capacity.
// Returns a heap-allocated IdentStack with no scopes.
// Caller must push scopes before lookup/insert.
struct IdentStack* create_ident_stack(size_t initial_capacity){
  struct IdentMap** maps = malloc(initial_capacity * sizeof(struct IdentMap*));
  struct IdentStack* stack = malloc(sizeof(struct IdentStack));

  stack->maps = maps;
  stack->size = 0;
  stack->capacity = initial_capacity;

  return stack;
}

// Push a map onto the scope stack, resizing if needed.
// Adds map to the top of the stack.
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

// Pop the top scope map from the stack.
// Returns the popped IdentMap or NULL if the stack is empty.
struct IdentMap* ident_stack_pop(struct IdentStack* stack){
  if (stack->size == 0){
    return NULL;
  } else {
    stack->size -= 1;
    return stack->maps[stack->size];
  }
}

// Peek at the current scope map without removing it.
// Returns the top IdentMap or NULL if the stack is empty.
// Returned map remains owned by the stack.
struct IdentMap* ident_stack_peek(struct IdentStack* stack){
  if (stack->size == 0){
    return NULL;
  } else {
    return stack->maps[stack->size - 1];
  }
}

// Look up an identifier across all scopes.
// Returns the entry and sets from_current_scope.
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

// Check if a name is declared in the current scope.
// Returns true if found in the top scope.
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

// Insert a name mapping into the current scope.
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

// Push a new empty scope onto the stack.
// Adds a new IdentMap with default bucket count.
void enter_scope(struct IdentStack* stack){
  struct IdentMap* new_map = create_ident_map(IDENT_MAP_BUCKETS);
  ident_stack_push(stack, new_map);
}

// Pop and destroy the current scope.
// Returns the top IdentMap and removes it from the stack.
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

// Destroy the entire identifier stack and all scopes.
void destroy_ident_stack(struct IdentStack* stack){
  for (int i = 0; i < stack->size; ++i){
    destroy_ident_map(stack->maps[i]);
  }
  free(stack->maps);
  free(stack);
}

// Allocate a new identifier map with a given bucket count.
// Returns a heap-allocated IdentMap.
// Caller must destroy the map when done.
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

// Allocate a new identifier map entry.
// Returns a heap-allocated IdentMapEntry.
// Key/entry_name pointers remain valid for entry lifetime.
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

// Insert or update a mapping within a bucket chain.
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

// Insert or update a mapping in the identifier map.
// Hash_slice produces stable bucket indices.
void ident_map_insert(struct IdentMap* hmap, struct Slice* key, 
    struct Slice* entry_name, bool has_linkage, enum TypeType type, bool is_const, unsigned value){
  size_t hash = hash_slice(key) % hmap->size;
  
  if ((hmap->arr[hash]) == NULL){
    hmap->arr[hash] = create_ident_map_entry(key, entry_name, has_linkage, type, is_const, value);
  } else {
    ident_map_entry_insert(hmap->arr[hash], key, entry_name, has_linkage, type, is_const, value);
  }
}

// Look up an identifier within a bucket chain.
// Returns the entry or NULL if missing.
struct IdentMapEntry* ident_map_entry_get(struct IdentMapEntry* entry, struct Slice* key){
  if (compare_slice_to_slice(entry->key, key)){
    return entry;
  } else if (entry->next == NULL){
    return NULL;
  } else {
    return ident_map_entry_get(entry->next, key);
  }
}

// Look up an identifier in the map.
// Returns the entry or NULL if missing.
struct IdentMapEntry* ident_map_get(struct IdentMap* hmap, struct Slice* key){
  size_t hash = hash_slice(key) % hmap->size;

  if (hmap->arr[hash] == NULL){
    return NULL;
  } else {
    return ident_map_entry_get(hmap->arr[hash], key);
  }
}

// Free a bucket chain of identifier entries.
void destroy_ident_map_entry(struct IdentMapEntry* entry){
  if (entry->next != NULL) destroy_ident_map_entry(entry->next);
  free(entry);
}

// Destroy an identifier map and its entries.
// Caller must not use hmap after destruction.
void destroy_ident_map(struct IdentMap* hmap){
  for (int i = 0; i < hmap->size; ++i){
    if (hmap->arr[i] != NULL) destroy_ident_map_entry(hmap->arr[i]);
  }
  free(hmap->arr);
  free(hmap);
}
