#include "../crt/assert.h"
#include "../crt/stdlib.h"
#include "hashmap.h"
#include "slice.h"

struct HashMap* create_hash_map(size_t num_buckets){
  struct HashEntry** arr = malloc(num_buckets * sizeof(struct HashEntry*));
  struct HashMap* hmap = malloc(sizeof(struct HashMap));

  for (int i = 0; i < num_buckets; ++i){
    arr[i] = NULL;
  }

  hmap->size = num_buckets;
  hmap->arr = arr;

  return hmap;
}

static void set_entry_section_info(struct HashEntry* entry, int section_index, bool is_section_relative){
  entry->section_index = section_index;
  entry->is_section_relative = is_section_relative;
}

static struct HashEntry* create_hash_entry(
  struct Slice* key,
  int value,
  bool is_def,
  bool is_data,
  int section_index,
  bool is_section_relative
){
  struct HashEntry* entry = malloc(sizeof(struct HashEntry));

  entry->key = key;
  entry->value = value;
  entry->is_defined = is_def;
  entry->is_data = is_data;
  set_entry_section_info(entry, section_index, is_section_relative);
  entry->next = NULL;

  return entry;
}

static void hash_entry_insert(
  struct HashEntry* entry,
  struct Slice* key,
  int value,
  bool is_def,
  bool is_data,
  int section_index,
  bool is_section_relative
){
  if (compare_slice_to_slice(entry->key, key)){
    entry->value = value;
    entry->is_defined = is_def;
    entry->is_data = is_data;
    set_entry_section_info(entry, section_index, is_section_relative);
    free(key);
  } else if (entry->next == NULL){
    entry->next = create_hash_entry(key, value, is_def, is_data, section_index, is_section_relative);
  } else {
    hash_entry_insert(entry->next, key, value, is_def, is_data, section_index, is_section_relative);
  }
}

void hash_map_insert(struct HashMap* hmap, struct Slice* key, int value, bool is_def, bool is_data){
  hash_map_insert_with_section(hmap, key, value, is_def, is_data, -1, false);
}

void hash_map_insert_with_section(
  struct HashMap* hmap,
  struct Slice* key,
  int value,
  bool is_def,
  bool is_data,
  int section_index,
  bool is_section_relative
){
  size_t hash = hash_slice(key) % hmap->size;
  
  if ((hmap->arr[hash]) == NULL){
    hmap->arr[hash] = create_hash_entry(
      key,
      value,
      is_def,
      is_data,
      section_index,
      is_section_relative
    );
  } else {
    hash_entry_insert(
      hmap->arr[hash],
      key,
      value,
      is_def,
      is_data,
      section_index,
      is_section_relative
    );
  }
}

static int hash_entry_get(struct HashEntry* entry, struct Slice* key){
  if (compare_slice_to_slice(entry->key, key)){
    return entry->value;
  } else if (entry->next == NULL){
    return 0;
  } else {
    return hash_entry_get(entry->next, key);
  }
}

static struct HashEntry* find_hash_entry(struct HashEntry* entry, struct Slice* key){
  if (compare_slice_to_slice(entry->key, key)){
    return entry;
  } else if (entry->next == NULL){
    return NULL;
  } else {
    return find_hash_entry(entry->next, key);
  }
}

int hash_map_get(struct HashMap* hmap, struct Slice* key){
  size_t hash = hash_slice(key) % hmap->size;

  if (hmap->arr[hash] == NULL){
    return 0;
  } else {
    return hash_entry_get(hmap->arr[hash], key);
  }
}

struct HashEntry* hash_map_find_entry(struct HashMap* hmap, struct Slice* key){
  size_t hash = hash_slice(key) % hmap->size;

  if (hmap->arr[hash] == NULL){
    return NULL;
  } else {
    return find_hash_entry(hmap->arr[hash], key);
  }
}

bool hash_entry_contains(struct HashEntry* entry, struct Slice* key){
  if (compare_slice_to_slice(entry->key, key)){
    return true;
  } else if (entry->next == NULL){
    return false;
  } else {
    return hash_entry_contains(entry->next, key);
  }
}

bool hash_entry_contains_def(struct HashEntry* entry, struct Slice* key){
  if (compare_slice_to_slice(entry->key, key) && entry->is_defined){
    return true;
  } else if (entry->next == NULL){
    return false;
  } else {
    return hash_entry_contains_def(entry->next, key);
  }
}

bool hash_map_contains(struct HashMap* hmap, struct Slice* key){
  size_t hash = hash_slice(key) % hmap->size;

  if (hmap->arr[hash] == NULL){
    return false;
  } else {
    return hash_entry_contains(hmap->arr[hash], key);
  }
}

bool label_has_definition(struct HashMap* hmap, struct Slice* key){
  size_t hash = hash_slice(key) % hmap->size;

  if (hmap->arr[hash] == NULL){
    return false;
  } else {
    return hash_entry_contains_def(hmap->arr[hash], key);
  }
}

static void make_entry_defined(
  struct HashEntry* entry,
  struct Slice* key,
  int value,
  int section_index,
  bool is_section_relative
){
  if (compare_slice_to_slice(entry->key, key)){
    entry->is_defined = true;
    entry->value = value;
    set_entry_section_info(entry, section_index, is_section_relative);
  } else {
    assert(entry->next != NULL, "hashmap definition chain terminated early");
    make_entry_defined(entry->next, key, value, section_index, is_section_relative);
  }
}

void make_defined(struct HashMap* hmap, struct Slice* key, int value){
  make_defined_with_section(hmap, key, value, -1, false);
}

void make_defined_with_section(
  struct HashMap* hmap,
  struct Slice* key,
  int value,
  int section_index,
  bool is_section_relative
){
  size_t hash = hash_slice(key) % hmap->size;

  assert(hmap->arr[hash] != NULL, "hashmap entry missing while marking defined");

  make_entry_defined(hmap->arr[hash], key, value, section_index, is_section_relative);
}

void destroy_hash_entry(struct HashEntry* entry){
  if (entry->next !=  NULL) destroy_hash_entry(entry->next);
  free(entry->key);
  free(entry);
}

void destroy_hash_map(struct HashMap* hmap){
  for (int i = 0; i < hmap->size; ++i){
    if (hmap->arr[i] != NULL) destroy_hash_entry(hmap->arr[i]);
  }
  free(hmap->arr);
  free(hmap);
}
