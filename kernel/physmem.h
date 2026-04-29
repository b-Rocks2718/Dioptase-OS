#ifndef PHYSMEM_H
#define PHYSMEM_H

#include "blocking_lock.h"
#include "semaphore.h"

#define FRAME_SIZE 4096

#define FRAMES_ADDR_START 0x800000
#define FRAMES_ADDR_END   0x7FB8000

#define PHYS_FRAME_COUNT 30648

#define PHYS_FRAME_MAX_ORDER 14
// because my macro support is bad :(
#define PHYS_FRAME_MAX_ORDER_PLUS_ONE 15

#define FREE_PAGE_BITMAP_SIZE 3831 // PHYS_FRAME_COUNT / 8 rounded up

#define LOCAL_CACHE_SIZE 16

struct PhysmemLocalCache {
  void* pages[LOCAL_CACHE_SIZE];
  unsigned count;
  struct BlockingLock lock;
};

// free pages store metadata to form a linked list
struct FreePageNode {
  struct FreePageNode* prev;
  struct FreePageNode* next;
  unsigned free_order;
};

// initialize physical page allocator
void physmem_init(void);

// get the frame index corresponding to a physical address (first frame is index 0)
unsigned frame_index_from_address(unsigned phys_addr);

// get the physical address corresponding to a frame index (first frame is index 0)
unsigned address_from_frame_index(unsigned frame_index);

// allocate a physical page of given order
// Panics if no free frames remain
void* physmem_alloc_order(int order);

// allocate a physical page of given order and count it as intentionally leaked for leak reporting
void* physmem_leak_order(int order);

// free a physical page of given order
void physmem_free_order(void* page, int order);

// allocate a physical page
// Panics if no free frames remain
void* physmem_alloc(void);

// free a physical page
void physmem_free(void* page);

// check for physical memory leaks
void physmem_check_leaks(void);

enum PageFlags {
  PG_PINNED = 0x1,   // Non-evictable: either is being managed by physmem, or not able to be evicted (but owned by exactly one thing)
  PG_ACCESSED = 0x2, // Software-managed access bit
  PG_DIRTY = 0x4,    // Has been written to since last writeback
};

struct PageRef;
struct PageCacheEntry;
struct VME;

struct Page {
  unsigned flags;
  unsigned ref_cnt; // ref_cnt == len(refs)
  struct PageRef* refs;
  struct PageCacheEntry* cache_entry;
  struct Semaphore lock;
};

// static_assert(sizeof(struct Page) == 64, "Page is unexpected size; physmem assembly will be sad");

// Get the metadata from a frame physical address
struct Page* get_page(void* frame);

// Set and clear flags (does not acquire lock)
void physmem_set_page_flags(struct Page* page, unsigned flags);
void physmem_clear_page_flags(struct Page* page, unsigned flags);

// Lock and unlock page
void physmem_page_lock(struct Page* page);
bool physmem_page_trylock(struct Page* page);
void physmem_page_unlock(struct Page* page);

// Add and remove PageRefs (defaults to current process; does not acquire lock)
void physmem_page_addRef(struct Page* page, unsigned virtual_addr);
void physmem_page_removeRef(struct Page* page, unsigned virtual_addr, unsigned pid);

extern void physmem_metadata_init(struct Page* physmem_map, unsigned frame_count, unsigned pg_init_flags);
#endif // PHYSMEM_H
