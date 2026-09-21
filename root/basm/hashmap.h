#ifndef HASHMAP_H
#define HASHMAP_H

#include "../crt/stdbool.h"

#include "slice.h"

// Store one symbol key, value, definition state, and section metadata.
struct HashEntry{
  struct Slice* key;
  int value;
  bool is_defined;
  bool is_data;
  // During assembler pass 1, labels are stored as section-relative offsets until
  // final section bases are known. Defines and fully resolved labels clear this.
  bool is_section_relative;
  int section_index;
  struct HashEntry* next;
};

// Own the bucket array used for assembler symbol lookup.
struct HashMap{
	unsigned size;
  struct HashEntry** arr;
};

struct HashMap* create_hash_map(unsigned numBuckets);

void hash_map_insert(struct HashMap* hmap, struct Slice* key, int value, bool is_def, bool is_data);

// Insert a symbol whose value is section-relative during pass 1.
// Section_index identifies the section owning value; is_section_relative
//         distinguishes raw section offsets from already absolute values.
// Insert or replace a symbol while preserving its definition metadata.
// Section_index is only meaningful when
// is_section_relative is true.
void hash_map_insert_with_section(
  struct HashMap* hmap,
  struct Slice* key,
  int value,
  bool is_def,
  bool is_data,
  int section_index,
  bool is_section_relative
);

int hash_map_get(struct HashMap* hmap, struct Slice* key);

struct HashEntry* hash_map_find_entry(struct HashMap* hmap, struct Slice* key);

bool hash_map_contains(struct HashMap* hmap, struct Slice* key);

bool label_has_definition(struct HashMap* hmap, struct Slice* key);

void destroy_hash_map(struct HashMap* hmap);

void make_defined(struct HashMap* map, struct Slice* key, int value);

void make_defined_with_section(
  struct HashMap* map,
  struct Slice* key,
  int value,
  int section_index,
  bool is_section_relative
);

#endif  // HASHMAP_H
