#include "hashmap.h"
#include "heap.h"

void hash_map_init(struct HashMap* hmap, unsigned num_buckets){
  struct HashEntry** arr = malloc(num_buckets * sizeof(struct HashEntry*));

  for (int i = 0; i < num_buckets; ++i){
    arr[i] = NULL;
  }

  hmap->size = num_buckets;
  hmap->arr = arr;
}

// allocate one detached bucket-chain entry
static struct HashEntry* create_hash_entry(unsigned key, void* value){
  struct HashEntry* entry = malloc(sizeof(struct HashEntry));

  entry->key = key;
  entry->value = value;
  entry->next = NULL;

  return entry;
}

// walk one collision chain, updating the existing key or appending a new tail
static void* hash_entry_insert(struct HashEntry* entry, unsigned key, void* value){
  while (true){
    if (entry->key == key){
      void* old_value = entry->value;
      entry->value = value;
      return old_value;
    }

    if (entry->next == NULL){
      entry->next = create_hash_entry(key, value);
      return NULL;
    }

    entry = entry->next;
  }
}

void* hash_map_insert(struct HashMap* hmap, unsigned key, void* value){
  unsigned hash = key % hmap->size;
  
  if ((hmap->arr[hash]) == NULL){
    hmap->arr[hash] = create_hash_entry(key, value);
    return NULL;
  } else {
    return hash_entry_insert(hmap->arr[hash], key, value);
  }
}

// walk one collision chain without overwriting an existing mapping
void* hash_entry_try_insert(struct HashEntry* entry, unsigned key, void* value){
  while (true){
    if (entry->key == key){
      return entry->value;
    }

    if (entry->next == NULL){
      entry->next = create_hash_entry(key, value);
      return NULL;
    }

    entry = entry->next;
  }
}

void* hash_map_try_insert(struct HashMap* hmap, unsigned key, void* value){
  unsigned hash = key % hmap->size;
  
  if ((hmap->arr[hash]) == NULL){
    hmap->arr[hash] = create_hash_entry(key, value);
    return NULL;
  } else {
    return hash_entry_try_insert(hmap->arr[hash], key, value);
  }
}

static void* hash_entry_get(struct HashEntry* entry, unsigned key){
  while (entry != NULL){
    if (entry->key == key){
      return entry->value;
    }

    entry = entry->next;
  }

  return NULL;
}

void* hash_map_get(struct HashMap* hmap, unsigned key){
  unsigned hash = key % hmap->size;

  if (hmap->arr[hash] == NULL){
    return NULL;
  } else {
    return hash_entry_get(hmap->arr[hash], key);
  }
}

static bool hash_entry_contains(struct HashEntry* entry, unsigned key){
  while (entry != NULL){
    if (entry->key == key){
      return true;
    }

    entry = entry->next;
  }

  return false;
}

bool hash_map_contains(struct HashMap* hmap, unsigned key){
  unsigned hash = key % hmap->size;

  if (hmap->arr[hash] == NULL){
    return false;
  } else {
    return hash_entry_contains(hmap->arr[hash], key);
  }
}

// remove one key from a bucket chain by relinking through the previous next pointer
static void* hash_entry_remove(struct HashEntry** head, unsigned key){
  while (*head != NULL){
    struct HashEntry* entry = *head;

    if (entry->key == key){
      void* value = entry->value;
      *head = entry->next;
      free(entry);
      return value;
    }

    head = &entry->next;
  }

  return NULL;
}

void* hash_map_remove(struct HashMap* hmap, unsigned key){
  unsigned hash = key % hmap->size;

  if (hmap->arr[hash] == NULL){
    return NULL;
  } else {
    return hash_entry_remove(&hmap->arr[hash], key);
  }
}

// free an entire collision chain
static void hash_entry_free(struct HashEntry* entry){
  while (entry != NULL){
    struct HashEntry* next = entry->next;
    free(entry);
    entry = next;
  }
}

void hash_map_destroy(struct HashMap* hmap){
  for (int i = 0; i < hmap->size; ++i){
    if (hmap->arr[i] != NULL) hash_entry_free(hmap->arr[i]);
  }
  free(hmap->arr);
}

void hash_map_free(struct HashMap* hmap){
  hash_map_destroy(hmap);
  free(hmap);
}
