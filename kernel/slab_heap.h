#ifndef SLAB_HEAP
#define SLAB_HEAP

#include "blocking_lock.h"

struct Slab {
  void* free_list; // Pointer to the first free object in the slab
  unsigned object_size;
  unsigned free_objects;
  struct Slab* next;
  struct Slab* prev;
};

struct SlabCache {
  struct BlockingLock lock;
  unsigned objects_per_slab;
  struct Slab* full_slabs;
  struct Slab* partial_slabs;
  struct Slab* empty_slabs;
};

struct FreeObject {
  struct FreeObject* next;
};

void slab_heap_init();

void* slab_heap_alloc(unsigned size);

void slab_heap_free(void* ptr);

#endif // SLAB_HEAP
