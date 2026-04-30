#include "label_map.h"

#include "../crt/stdlib.h"
#include "../crt/stdbool.h"
#include "../crt/assert.h"

#include "slice.h"

// Purpose: Implement a simple hash map for label resolution.
// Inputs: Keys and values are slices (not copied).
// Outputs: Provides create/insert/lookup/destroy operations.
// Invariants/Assumptions: Caller owns slice storage; map manages entry nodes.

// Purpose: Allocate and initialize a label map.
// Inputs: num_buckets is the number of hash buckets.
// Outputs: Returns a heap-allocated LabelMap.
// Invariants/Assumptions: Caller must destroy the map to avoid leaks.
struct LabelMap* create_label_map(size_t num_buckets){
  struct LabelEntry** arr = malloc(num_buckets * sizeof(struct LabelEntry*));
  struct LabelMap* hmap = malloc(sizeof(struct LabelMap));

  for (int i = 0; i < num_buckets; ++i){
    arr[i] = NULL;
  }

  hmap->size = num_buckets;
  hmap->arr = arr;

  return hmap;
}

// Purpose: Create a label map entry node.
// Inputs: key/value are label slices.
// Outputs: Returns a heap-allocated LabelEntry.
// Invariants/Assumptions: key/value pointers remain valid for entry lifetime.
struct LabelEntry* create_label_entry(struct Slice* key, struct Slice* value){
  struct LabelEntry* entry = malloc(sizeof(struct LabelEntry));

  entry->key = key;
  entry->value = value;
  entry->next = NULL;

  return entry;
}

// Purpose: Insert or update a key/value mapping in a bucket chain.
// Inputs: entry is the head of a chain; key/value are the mapping to add.
// Outputs: Updates the chain in place.
// Invariants/Assumptions: Uses recursive traversal; chain is singly linked.
void label_entry_insert(struct LabelEntry* entry, struct Slice* key, struct Slice* value){
  if (compare_slice_to_slice(entry->key, key)){
    entry->value = value;
  } else if (entry->next == NULL){
    entry->next = create_label_entry(key, value);
  } else {
    label_entry_insert(entry->next, key, value);
  }
}

// Purpose: Insert or update a mapping in the label map.
// Inputs: hmap is the target map; key/value are the mapping to add.
// Outputs: Updates the map in place.
// Invariants/Assumptions: hash_slice produces a stable hash for the key.
void label_map_insert(struct LabelMap* hmap, struct Slice* key, struct Slice* value){
  size_t label = hash_slice(key) % hmap->size;
  
  if ((hmap->arr[label]) == NULL){
    hmap->arr[label] = create_label_entry(key, value);
  } else {
    label_entry_insert(hmap->arr[label], key, value);
  }
}

// Purpose: Look up a key within a bucket chain.
// Inputs: entry is the head of the chain; key is the lookup label.
// Outputs: Returns the mapped value or NULL if missing.
// Invariants/Assumptions: Chain nodes were created by create_label_entry.
struct Slice* label_entry_get(struct LabelEntry* entry, struct Slice* key){
  if (compare_slice_to_slice(entry->key, key)){
    return entry->value;
  } else if (entry->next == NULL){
    return 0;
  } else {
    return label_entry_get(entry->next, key);
  }
}

// Purpose: Look up a key in the label map.
// Inputs: hmap is the map; key is the lookup label.
// Outputs: Returns the mapped value or NULL if missing.
// Invariants/Assumptions: hash_slice is consistent with insertions.
struct Slice* label_map_get(struct LabelMap* hmap, struct Slice* key){
  size_t label = hash_slice(key) % hmap->size;

  if (hmap->arr[label] == NULL){
    return 0;
  } else {
    return label_entry_get(hmap->arr[label], key);
  }
}

// Purpose: Check whether a key exists in a bucket chain.
// Inputs: entry is the chain head; key is the lookup label.
// Outputs: Returns true if the key is present.
// Invariants/Assumptions: Chain nodes use compare_slice_to_slice for equality.
bool label_entry_contains(struct LabelEntry* entry, struct Slice* key){
  if (compare_slice_to_slice(entry->key, key)){
    return true;
  } else if (entry->next == NULL){
    return false;
  } else {
    return label_entry_contains(entry->next, key);
  }
}

// Purpose: Check whether a key exists in the map.
// Inputs: hmap is the map; key is the lookup label.
// Outputs: Returns true if the key is present.
// Invariants/Assumptions: hash_slice distributes keys across buckets.
bool label_map_contains(struct LabelMap* hmap, struct Slice* key){
  size_t label = hash_slice(key) % hmap->size;

  if (hmap->arr[label] == NULL){
    return false;
  } else {
    return label_entry_contains(hmap->arr[label], key);
  }
}

// Purpose: Free a bucket chain of label entries.
// Inputs: entry is the head of the chain.
// Outputs: Frees each LabelEntry node.
// Invariants/Assumptions: Does not free slices referenced by entries.
void destroy_label_entry(struct LabelEntry* entry){
  if (entry->next !=  NULL) destroy_label_entry(entry->next);
  free(entry);
}

// Purpose: Destroy a label map and all of its entries.
// Inputs: hmap is the map to free.
// Outputs: Frees bucket storage and entry nodes.
// Invariants/Assumptions: Caller must not use hmap after destruction.
void destroy_label_map(struct LabelMap* hmap){
  for (int i = 0; i < hmap->size; ++i){
    if (hmap->arr[i] != NULL) destroy_label_entry(hmap->arr[i]);
  }
  free(hmap->arr);
  free(hmap);
}
