#ifndef LABEL_MAP_H
#define LABEL_MAP_H

#include "../crt/stdint.h"

#include "slice.h"

// Provide a simple hash map from label slices to label slices.
// Keys and values are slices owned by the caller or arena.
// Supports insert/lookup for goto label resolution.

// Node for a single hash bucket chain.
// Returns Stored inside LabelMap buckets.
// Key/value pointers remain valid for map lifetime.
struct LabelEntry{
  struct Slice* key;
  struct Slice* value;
  struct LabelEntry* next;
};

// Hash map from label name to resolved label.
// Used to resolve goto labels within a function.
struct LabelMap{
	size_t size;
  struct LabelEntry** arr;
};

// Allocate a label map with the given bucket count.
// Returns an allocated LabelMap.
// Caller must destroy the map with destroy_label_map.
struct LabelMap* create_label_map(size_t numBuckets);

// Insert or update a key/value mapping in the map.
void label_map_insert(struct LabelMap* hmap, struct Slice* key, struct Slice* value);

// Look up a key in the map.
// Returns the mapped value or NULL if missing.
struct Slice* label_map_get(struct LabelMap* hmap, struct Slice* key);

// Check whether a key exists in the map.
// Returns true if the key is present.
bool label_map_contains(struct LabelMap* hmap, struct Slice* key);

// Free all map entries and bucket storage.
// Does not free slices owned by the caller.
void destroy_label_map(struct LabelMap* hmap);

#endif // LABEL_MAP_H
