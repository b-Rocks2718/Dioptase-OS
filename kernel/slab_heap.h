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
  struct Slab* empty_slabs; // keep max of 2 empty slabs
  unsigned num_empty_slabs; 
};

struct FreeObject {
  struct FreeObject* next;
};

#define NUM_OBJECT_SIZES 9
extern unsigned OBJECT_SIZES[NUM_OBJECT_SIZES];

#define MIN_PER_CORE_FREE_LIST 0
#define MAX_PER_CORE_FREE_LIST 64
#define PER_CORE_FREE_LIST_REFILL 32

void slab_heap_init();

void* slab_heap_alloc(unsigned size);

void slab_heap_free(void* ptr);

void slab_heap_destroy();

#endif // SLAB_HEAP
