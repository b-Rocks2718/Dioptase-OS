#include "label_map.h"

#include "../crt/stdlib.h"
#include "../crt/stdbool.h"
#include "../crt/assert.h"

#include "slice.h"

// Implement a simple hash map for label resolution.
// Provides create/insert/lookup/destroy operations.
// Caller owns slice storage; map manages entry nodes.

// Allocate and initialize a label map.
// Returns a heap-allocated LabelMap.
// Caller must destroy the map to avoid leaks.
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

// Allocate one label-map collision-chain entry.
// Returns a heap-allocated LabelEntry.
// Key/value pointers remain valid for entry lifetime.
struct LabelEntry* create_label_entry(struct Slice* key, struct Slice* value){
  struct LabelEntry* entry = malloc(sizeof(struct LabelEntry));

  entry->key = key;
  entry->value = value;
  entry->next = NULL;

  return entry;
}

// Insert or update a key/value mapping in a bucket chain.
void label_entry_insert(struct LabelEntry* entry, struct Slice* key, struct Slice* value){
  if (compare_slice_to_slice(entry->key, key)){
    entry->value = value;
  } else if (entry->next == NULL){
    entry->next = create_label_entry(key, value);
  } else {
    label_entry_insert(entry->next, key, value);
  }
}

// Insert or update a mapping in the label map.
// Hash_slice produces a stable hash for the key.
void label_map_insert(struct LabelMap* hmap, struct Slice* key, struct Slice* value){
  size_t label = hash_slice(key) % hmap->size;
  
  if ((hmap->arr[label]) == NULL){
    hmap->arr[label] = create_label_entry(key, value);
  } else {
    label_entry_insert(hmap->arr[label], key, value);
  }
}

// Look up a key within a bucket chain.
// Returns the mapped value or NULL if missing.
struct Slice* label_entry_get(struct LabelEntry* entry, struct Slice* key){
  if (compare_slice_to_slice(entry->key, key)){
    return entry->value;
  } else if (entry->next == NULL){
    return 0;
  } else {
    return label_entry_get(entry->next, key);
  }
}

// Look up a key in the label map.
// Returns the mapped value or NULL if missing.
struct Slice* label_map_get(struct LabelMap* hmap, struct Slice* key){
  size_t label = hash_slice(key) % hmap->size;

  if (hmap->arr[label] == NULL){
    return 0;
  } else {
    return label_entry_get(hmap->arr[label], key);
  }
}

// Check whether a key exists in a bucket chain.
// Returns true if the key is present.
bool label_entry_contains(struct LabelEntry* entry, struct Slice* key){
  if (compare_slice_to_slice(entry->key, key)){
    return true;
  } else if (entry->next == NULL){
    return false;
  } else {
    return label_entry_contains(entry->next, key);
  }
}

// Check whether a key exists in the map.
// Returns true if the key is present.
bool label_map_contains(struct LabelMap* hmap, struct Slice* key){
  size_t label = hash_slice(key) % hmap->size;

  if (hmap->arr[label] == NULL){
    return false;
  } else {
    return label_entry_contains(hmap->arr[label], key);
  }
}

// Free a bucket chain of label entries.
void destroy_label_entry(struct LabelEntry* entry){
  if (entry->next !=  NULL) destroy_label_entry(entry->next);
  free(entry);
}

// Destroy a label map and all of its entries.
// Caller must not use hmap after destruction.
void destroy_label_map(struct LabelMap* hmap){
  for (int i = 0; i < hmap->size; ++i){
    if (hmap->arr[i] != NULL) destroy_label_entry(hmap->arr[i]);
  }
  free(hmap->arr);
  free(hmap);
}
