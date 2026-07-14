#ifndef HEAP_H
#define HEAP_H

#include "blocking_lock.h"

#define HEAP_POISON 0xABCDEFAA

struct Slab {
  void* free_list; // Pointer to the first free object in the slab
  unsigned object_size;
  unsigned free_objects;
  struct Slab* next;
  struct Slab* prev;

  #ifdef HEAP_DEBUG
  char allocation_bitmap[4]; // variable size struct
    // we may reserve > 4 bytes for the bitmap
  #endif
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

void heap_init();

void heap_sync_init();

extern void heap_large_alloc_init(unsigned char* large_allocation_orders,
  int phys_frame_count, int heap_large_alloc_none); // large loop, done in asm

void* malloc(unsigned size);

void* leak(unsigned size);

void free(void* ptr);

void heap_destroy();

#endif // HEAP_H
