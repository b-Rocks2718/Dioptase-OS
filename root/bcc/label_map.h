#ifndef LABEL_MAP_H
#define LABEL_MAP_H

#include "../crt/stdint.h"

#include "slice.h"

// Purpose: Provide a simple hash map from label slices to label slices.
// Inputs: Keys and values are slices owned by the caller or arena.
// Outputs: Supports insert/lookup for goto label resolution.
// Invariants/Assumptions: Map does not copy slices; it stores pointers.

// Purpose: Node for a single hash bucket chain.
// Inputs: key/value are label slices; next links collisions.
// Outputs: Stored inside LabelMap buckets.
// Invariants/Assumptions: key/value pointers remain valid for map lifetime.
struct LabelEntry{
  struct Slice* key;
  struct Slice* value;
  struct LabelEntry* next;
};

// Purpose: Hash map from label name to resolved label.
// Inputs: size is the bucket count; arr stores chains of LabelEntry.
// Outputs: Used to resolve goto labels within a function.
// Invariants/Assumptions: size is non-zero; arr length equals size.
struct LabelMap{
	size_t size;
  struct LabelEntry** arr;
};

// Purpose: Allocate a label map with the given bucket count.
// Inputs: numBuckets is the number of hash buckets.
// Outputs: Returns an allocated LabelMap.
// Invariants/Assumptions: Caller must destroy the map with destroy_label_map.
struct LabelMap* create_label_map(size_t numBuckets);

// Purpose: Insert or update a key/value mapping in the map.
// Inputs: hmap is the map; key/value are label slices.
// Outputs: Updates the map in place.
// Invariants/Assumptions: Map owns the entry nodes but not the slices.
void label_map_insert(struct LabelMap* hmap, struct Slice* key, struct Slice* value);

// Purpose: Look up a key in the map.
// Inputs: hmap is the map; key is the lookup label.
// Outputs: Returns the mapped value or NULL if missing.
// Invariants/Assumptions: Caller handles missing entries.
struct Slice* label_map_get(struct LabelMap* hmap, struct Slice* key);

// Purpose: Check whether a key exists in the map.
// Inputs: hmap is the map; key is the lookup label.
// Outputs: Returns true if the key is present.
// Invariants/Assumptions: Does not distinguish between missing and NULL values.
bool label_map_contains(struct LabelMap* hmap, struct Slice* key);

// Purpose: Free all map entries and bucket storage.
// Inputs: hmap is the map to destroy.
// Outputs: Frees heap memory for the map and its entries.
// Invariants/Assumptions: Does not free slices owned by the caller.
void destroy_label_map(struct LabelMap* hmap);

#endif // LABEL_MAP_H
