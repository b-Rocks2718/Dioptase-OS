#ifndef HEAP_H
#define HEAP_H

#include "blocking_lock.h"

#define HEAP_POISON 0xABCDEFAA

// Track one fixed-size object slab and its allocation bitmap.
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

// Track all slabs serving one object size on one core.
struct SlabCache {
  struct BlockingLock lock;
  unsigned objects_per_slab;
  struct Slab* full_slabs;
  struct Slab* partial_slabs;
  struct Slab* empty_slabs; // keep max of 2 empty slabs
  unsigned num_empty_slabs;
};

// Link an unused slab object into its cache free list.
struct FreeObject {
  struct FreeObject* next;
};

#define NUM_OBJECT_SIZES 9
extern unsigned OBJECT_SIZES[NUM_OBJECT_SIZES];

#define MIN_PER_CORE_FREE_LIST 0
#define MAX_PER_CORE_FREE_LIST 64
#define PER_CORE_FREE_LIST_REFILL 32

// Initialize slab caches and per-core free lists. Runs once on core 0 before
// any allocation; the heap is unlocked until heap_sync_init().
void heap_init();

// Initialize heap locks once threading primitives are usable. Before this
// call the heap must only be used single-threaded during boot.
void heap_sync_init();

extern void heap_large_alloc_init(unsigned char* large_allocation_orders,
  int phys_frame_count, int heap_large_alloc_none); // large loop, done in asm

// Allocate size (> 0) bytes of reclaimable kernel memory. Never returns NULL:
// exhausting physical memory panics, so callers need no failure path.
void* malloc(unsigned size);

// Allocate memory that lives until shutdown and is never freed. Like malloc(),
// panics instead of returning NULL. Counted separately by leak checks.
void* leak(unsigned size);

// Release a malloc() allocation. free(NULL) is a no-op. Freeing anything else
// that malloc() did not return is a kernel bug (detected under HEAP_DEBUG).
void free(void* ptr);

// Tear down heap locks and return all slab frames to physmem at shutdown,
// after every other heap user has stopped.
void heap_destroy();

#endif // HEAP_H
