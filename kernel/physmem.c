#include "physmem.h"
#include "print.h"
#include "debug.h"
#include "atomic.h"
#include "constants.h"
#include "blocking_lock.h"
#include "per_core.h"
#include "threads.h"

static struct BlockingLock physmem_lock;

static struct FreePageNode* free_page_list[PHYS_FRAME_MAX_ORDER_PLUS_ONE];
static unsigned char free_page_bitmap[FREE_PAGE_BITMAP_SIZE];

static int frames_alloced = 0;
static int frames_freed = 0;
static int frames_leaked = 0;

static int order_allocs[PHYS_FRAME_MAX_ORDER_PLUS_ONE];
static int order_frees[PHYS_FRAME_MAX_ORDER_PLUS_ONE];
static int order_leaks[PHYS_FRAME_MAX_ORDER_PLUS_ONE];

// sanity check that something could be a frame address
static bool physmem_is_frame_address(unsigned phys_addr) {
  if (phys_addr < FRAMES_ADDR_START || phys_addr >= FRAMES_ADDR_END) {
    return false;
  }
  return (phys_addr & (FRAME_SIZE - 1)) == 0;
}

unsigned frame_index_from_address(unsigned phys_addr) {
  assert(physmem_is_frame_address(phys_addr), "physmem: invalid frame address.\n");
  return (phys_addr - FRAMES_ADDR_START) / FRAME_SIZE;
}

unsigned address_from_frame_index(unsigned frame_index) {
  assert(frame_index < PHYS_FRAME_COUNT, "physmem: invalid frame index.\n");
  return FRAMES_ADDR_START + frame_index * FRAME_SIZE;
}

static bool is_block_free(unsigned block_index) {
  return free_page_bitmap[block_index / 8] & (1u << (block_index % 8));
}

static void mark_block_allocated(unsigned block_index) {
  free_page_bitmap[block_index / 8] &= ~(1u << (block_index % 8));
}

static void mark_block_free(unsigned block_index) {
  free_page_bitmap[block_index / 8] |= (1u << (block_index % 8));
}

// add block to head of free list for given order
static void free_list_push(struct FreePageNode* block_addr, int order) {
  assert(order >= 0 && order <= PHYS_FRAME_MAX_ORDER, "physmem: invalid block order.\n");
  assert(physmem_is_frame_address((unsigned)block_addr), "physmem: invalid block address.\n");

  unsigned block_index = frame_index_from_address((unsigned)block_addr);
  assert((block_index & ((1u << order) - 1)) == 0, 
    "physmem: block address is not aligned to its size.\n");

  struct FreePageNode* node = block_addr;
  node->prev = NULL;
  node->next = free_page_list[order];
  if (free_page_list[order] != NULL) {
    free_page_list[order]->prev = node;
  }
  free_page_list[order] = node;

  mark_block_free(block_index);
  node->free_order = order;
}

// remove and return block from head of free list for given order, or NULL if empty
static struct FreePageNode* free_list_pop(int order) {
  assert(order >= 0 && order <= PHYS_FRAME_MAX_ORDER, "physmem: invalid block order.\n");

  if (free_page_list[order] == NULL) {
    return NULL;
  }

  struct FreePageNode* node = free_page_list[order];
  free_page_list[order] = node->next;

  if (node->next != NULL) {
    node->next->prev = NULL;
  }
  node->next = NULL;
  node->prev = NULL;

  unsigned block_index = frame_index_from_address((unsigned)node);
  mark_block_allocated(block_index);

  return node;
}

// remove a specific block from free list for given order
static struct FreePageNode* free_list_remove(struct FreePageNode* node) {
  unsigned block_index = frame_index_from_address((unsigned)node);
  unsigned order = node->free_order;

  assert(order >= 0 && order <= PHYS_FRAME_MAX_ORDER, "physmem: invalid block order.\n");
  assert(node != NULL, "physmem: cannot remove NULL node from free list.\n");

  if (node->prev != NULL) {
    node->prev->next = node->next;
  } else {
    // node is head of list
    free_page_list[order] = node->next;
  }
  if (node->next != NULL) {
    node->next->prev = node->prev;
  }

  // mark block as allocated
  mark_block_allocated(block_index);

  node->prev = NULL;
  node->next = NULL;
  return node;
}

// add all frames to free lists, coalescing into larger blocks as much as possible
void physmem_init(void){
  assert((FRAMES_ADDR_END - FRAMES_ADDR_START) / FRAME_SIZE == PHYS_FRAME_COUNT, 
  "physmem init: frame count does not match address range.\n");
  assert((PHYS_FRAME_COUNT + 7) / 8 == FREE_PAGE_BITMAP_SIZE, 
    "physmem init: free page bitmap size is incorrect.\n");

  blocking_lock_init(&physmem_lock);
  for (int i = 0; i < PHYS_FRAME_MAX_ORDER_PLUS_ONE; i++) {
    free_page_list[i] = NULL;
  }

  unsigned frames_remaining = PHYS_FRAME_COUNT;
  unsigned next_frame_index = 0;
  while (frames_remaining > 0) {
    int order = PHYS_FRAME_MAX_ORDER;

    // find max order that will fit in the remaining frames
    while (order > 0 && (1u << order) > frames_remaining) {
      order--;
    }

    // find max order that is aligned with the next frame address
    while (order > 0 && ((next_frame_index & ((1u << order) - 1)) != 0)) {
      order--;
    }

    unsigned block_addr = address_from_frame_index(next_frame_index);

    free_list_push((struct FreePageNode*)block_addr, order);

    next_frame_index += (1u << order);
    frames_remaining -= (1u << order);
  }

  // init per core caches
  for (int i = 0; i < MAX_CORES; i++) {
    blocking_lock_init(&per_core_data[i].physmem_cache.lock);
    per_core_data[i].physmem_cache.count = 0;
    for (int j = 0; j < LOCAL_CACHE_SIZE; j++) {
      per_core_data[i].physmem_cache.pages[j] = NULL;
    }
  }
}

// allocate a physical page of given order
// Panics if no free frames remain
void* physmem_alloc_order(int order){
  assert(order >= 0 && order <= PHYS_FRAME_MAX_ORDER, "physmem alloc: invalid order.\n");

  blocking_lock_acquire(&physmem_lock);

  __atomic_fetch_add(&order_allocs[order], 1);

  // find smallest order large enough to satisfy the request
  int current_order = order;
  while (free_page_list[current_order] == NULL) {
    if (current_order >= PHYS_FRAME_MAX_ORDER) {
      blocking_lock_release(&physmem_lock);
      panic("physmem alloc: out of physical pages.\n");
      return NULL;
    }
    current_order++;
  }

  // split larger block until we get down to the requested order
  struct FreePageNode* node = free_list_pop(current_order);

  while (current_order > order) {
    current_order--;
    unsigned buddy_addr = address_from_frame_index(
      frame_index_from_address((unsigned)node) + (1u << current_order));
    struct FreePageNode* buddy = (struct FreePageNode*)buddy_addr;

    free_list_push(buddy, current_order);
  }

  blocking_lock_release(&physmem_lock);

  assert(
    physmem_is_frame_address((unsigned)node),
    "physmem alloc: free list returned an invalid frame address.\n"
  );

  return node;
}

void* physmem_leak_order(int order){
  void* page = physmem_alloc_order(order);
  __atomic_fetch_add(&order_leaks[order], 1);
  return page;
}

// free a physical page of given order
void physmem_free_order(void* page, int order){
  unsigned phys_addr = (unsigned)page;
  assert(page != NULL, "physmem free: page is NULL.\n");
  assert(
    physmem_is_frame_address(phys_addr),
    "physmem free: page is not a valid allocatable frame.\n"
  );

  assert(order >= 0 && order <= PHYS_FRAME_MAX_ORDER, "physmem free: invalid order.\n");
  assert((frame_index_from_address(phys_addr) & ((1u << order) - 1)) == 0, 
    "physmem free: page address is not aligned to its size.\n");

  blocking_lock_acquire(&physmem_lock);

  __atomic_fetch_add(&order_frees[order], 1);

  // coalesce with buddy blocks if possible
  unsigned block_index = frame_index_from_address(phys_addr);
  while (order < PHYS_FRAME_MAX_ORDER) {
    unsigned buddy_index = block_index ^ (1u << order);
    if (buddy_index >= PHYS_FRAME_COUNT) {
      break; // buddy is out of range, so stop coalescing
    }

    unsigned buddy_addr = address_from_frame_index(buddy_index);

    if (!is_block_free(buddy_index)) {
      break; // buddy is not free or not the same size, so stop coalescing
    }

    struct FreePageNode* buddy_node = (struct FreePageNode*)buddy_addr;
    if (buddy_node->free_order != order) {
      break; // buddy is not the same size, so stop coalescing
    }

    // remove buddy from free list
    struct FreePageNode* buddy = free_list_remove((struct FreePageNode*)buddy_addr);

    // update block index to the combined block
    if (buddy_index < block_index) {
      block_index = buddy_index;
    }

    order++;
  }

  // add the (possibly coalesced) block back to the free list
  unsigned block_addr = address_from_frame_index(block_index);
  free_list_push((struct FreePageNode*)block_addr, order);

  blocking_lock_release(&physmem_lock);
}

// allocate a physical page from core-local cache
void* physmem_alloc(void){
  enum CoreAffinity prev = core_pin();
  struct PerCore* per_core = get_per_core();

  // protect against re-entrance, needed because physmem_alloc_order can block
  blocking_lock_acquire(&per_core->physmem_cache.lock);

  __atomic_fetch_add(&frames_alloced, 1);

  // refill cache if necessary, then pop and return a page
  if (per_core->physmem_cache.count == 0) {
    // refill cache
    for (int i = 0; i < LOCAL_CACHE_SIZE; i++) {
      per_core->physmem_cache.pages[i] = physmem_alloc_order(0);
    }
    per_core->physmem_cache.count = LOCAL_CACHE_SIZE;
  }

  // pop from local cache
  per_core->physmem_cache.count--;
  void* page = per_core->physmem_cache.pages[per_core->physmem_cache.count];

  blocking_lock_release(&per_core->physmem_cache.lock);

  core_unpin(prev);

  return page;
}

void* physmem_leak(void* page){
  __atomic_fetch_add(&frames_leaked, 1);
  return physmem_alloc();
}

// free a physical page
void physmem_free(void* page){
  enum CoreAffinity prev = core_pin();
  struct PerCore* per_core = get_per_core();

  // protect against re-entrance
  blocking_lock_acquire(&per_core->physmem_cache.lock);

  __atomic_fetch_add(&frames_freed, 1);
  
  // push to local cache if there is room, otherwise free to global pool
  if (per_core->physmem_cache.count < LOCAL_CACHE_SIZE) {
    per_core->physmem_cache.pages[per_core->physmem_cache.count] = page;
    per_core->physmem_cache.count++;

    blocking_lock_release(&per_core->physmem_cache.lock);
    core_unpin(prev);
  } else {
    blocking_lock_release(&per_core->physmem_cache.lock);
    core_unpin(prev);

    physmem_free_order(page, 0);
  }
}

void physmem_check_leaks(void){
  bool all_good = true;

  if (frames_alloced != frames_freed + frames_leaked) {
    int args[3] = {frames_freed, frames_leaked, frames_alloced};
    say("| Warning: physmem leak detected: (freed:%d + leaked:%d) != alloced:%d\n", args);
    all_good = false;
  }

  // skip order 0 because some frames may still be in caches
  for (int order = 1; order <= PHYS_FRAME_MAX_ORDER; order++) {
    if (order_allocs[order] != order_frees[order] + order_leaks[order]) {
      int args[4] = {order, order_frees[order], order_leaks[order], order_allocs[order]};
      say("| Warning: physmem leak detected for order %d: (freed:%d + leaked:%d) != alloced:%d\n", args);
      all_good = false;
    }
  }
  
  if (all_good) {
    say("| No physmem leaks detected\n", NULL);
  }
}
