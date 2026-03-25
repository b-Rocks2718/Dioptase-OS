#ifndef HASHMAP_H
#define HASHMAP_H

#include "constants.h"

// one bucket-chain entry in the hash map
struct HashEntry{
  unsigned key;
  void* value;
  struct HashEntry* next;
};

// hash map keyed by unsigned integers, value is a void*
struct HashMap {
	unsigned size;
  struct HashEntry** arr;
};

// initialize a hash map with the given number of buckets
void hash_map_init(struct HashMap* hmap, unsigned num_buckets);

// Inserts the key-value pair into the hash map. If the key already exists,
// updates the value and returns the old value. Otherwise, returns NULL.
void* hash_map_insert(struct HashMap* hmap, unsigned key, void* value);

// Inserts the key-value pair into the hash map only if the key does not already exist
// If the key already exists, returns the existing value without updating it.
// Otherwise, returns NULL
void* hash_map_try_insert(struct HashMap* hmap, unsigned key, void* value);

// return the value for key, or NULL if the key is absent
void* hash_map_get(struct HashMap* hmap, unsigned key);

// report whether the map currently contains key
bool hash_map_contains(struct HashMap* hmap, unsigned key);

// remove key and return its value, or NULL if absent
void* hash_map_remove(struct HashMap* hmap, unsigned key);

// free all bucket chains, but not the HashMap struct itself
void hash_map_destroy(struct HashMap* hmap);

// destroy the map and free the HashMap struct
void hash_map_free(struct HashMap* hmap);

#endif // HASHMAP_H
