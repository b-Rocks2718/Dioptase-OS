#ifndef PHYSMEM_H
#define PHYSMEM_H

#define FRAME_SIZE 4096

#define FRAMES_ADDR_START 0x800000
#define FRAMES_ADDR_END 0x7FBD000

#define PHYS_FRAME_COUNT 30653

#define PHYS_FRAME_MAX_ORDER 14
// because my macro support is bad :(
#define PHYS_FRAME_MAX_ORDER_PLUS_ONE 15

#define FREE_PAGE_BITMAP_SIZE 3832 // PHYS_FRAME_COUNT / 8 rounded up

#define LOCAL_CACHE_SIZE 16

struct PhysmemLocalCache {
  void* pages[LOCAL_CACHE_SIZE];
  unsigned count;
};

// free pages store metadata to form a linked list
struct FreePageNode {
  struct FreePageNode *prev;
  struct FreePageNode *next;
  unsigned free_order;
};

// initialize physical page allocator
void physmem_init(void);

// allocate a physical page of given order
// Panics if no free frames remain
void* physmem_alloc_order(int order);

// free a physical page of given order
void physmem_free_order(void* page, int order);

// allocate a physical page
// Panics if no free frames remain
void* physmem_alloc(void);

// free a physical page
void physmem_free(void* page);

#endif // PHYSMEM_H
