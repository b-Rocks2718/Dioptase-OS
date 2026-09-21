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

static bool physmem_sync_initialized = false;

// sanity check that something could be a frame address
static bool physmem_is_frame_address(unsigned phys_addr) {
  if (phys_addr < FRAMES_ADDR_START || phys_addr >= FRAMES_ADDR_END) {
    return false;
  }
  return (phys_addr & (FRAME_SIZE - 1)) == 0;
}

// Convert a validated physical frame address to its frame-table index.
unsigned frame_index_from_address(unsigned phys_addr) {
  assert_always(physmem_is_frame_address(phys_addr), "physmem: invalid frame address.\n");
  return (phys_addr - FRAMES_ADDR_START) / FRAME_SIZE;
}

// Convert a physical frame-table index to its byte address.
unsigned address_from_frame_index(unsigned frame_index) {
  assert_always(frame_index < PHYS_FRAME_COUNT, "physmem: invalid frame index.\n");
  return FRAMES_ADDR_START + frame_index * FRAME_SIZE;
}

// Return whether a buddy block's allocation bitmap marks it free.
static bool is_block_free(unsigned block_index) {
  return free_page_bitmap[block_index / 8] & (1u << (block_index % 8));
}

// Mark a buddy block allocated in the physical-memory bitmap.
static void mark_block_allocated(unsigned block_index) {
  free_page_bitmap[block_index / 8] &= ~(1u << (block_index % 8));
}

// Mark a buddy block free in the physical-memory bitmap.
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
  assert_always((FRAMES_ADDR_END - FRAMES_ADDR_START) / FRAME_SIZE == PHYS_FRAME_COUNT, 
  "physmem init: frame count does not match address range.\n");
  assert_always((PHYS_FRAME_COUNT + 7) / 8 == FREE_PAGE_BITMAP_SIZE, 
    "physmem init: free page bitmap size is incorrect.\n");

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
    per_core_data[i].physmem_cache.count = 0;
    for (int j = 0; j < LOCAL_CACHE_SIZE; j++) {
      per_core_data[i].physmem_cache.pages[j] = NULL;
    }
  }
}

// Initialize physical-memory locks after the free lists are built.
void physmem_sync_init(void){
  blocking_lock_init(&physmem_lock);
  for (int i = 0; i < MAX_CORES; i++) {
    blocking_lock_init(&per_core_data[i].physmem_cache.lock);
  }
  physmem_sync_initialized = true;
}

// to be called only from kernel_shutdown
void physmem_destroy_locks(void){
  bool locks_initialized = physmem_sync_initialized;

  // kernel_shutdown() has already stopped concurrent physmem users. Turn off
  // locking before freeing the CLH tail nodes backing these locks because those
  // frees go through the heap, and heap_destroy() will still need to return
  // pages to physmem after this point.
  physmem_sync_initialized = false;

  if (!locks_initialized) {
    return;
  }

  blocking_lock_destroy(&physmem_lock);
  for (int i = 0; i < MAX_CORES; i++) {
    blocking_lock_destroy(&per_core_data[i].physmem_cache.lock);
  }
}

// allocate a physical page of given order
// Returns NULL if no free frames remain; callers at public boundaries must
// translate that into a normal failure rather than treating it as corruption.
void* physmem_alloc_order(int order){
  assert(order >= 0 && order <= PHYS_FRAME_MAX_ORDER, "physmem alloc: invalid order.\n");

  if (physmem_sync_initialized) blocking_lock_acquire(&physmem_lock);

  __atomic_fetch_add(&order_allocs[order], 1);

  // find smallest order large enough to satisfy the request
  int current_order = order;
  while (free_page_list[current_order] == NULL) {
    if (current_order >= PHYS_FRAME_MAX_ORDER) {
      if (physmem_sync_initialized) blocking_lock_release(&physmem_lock);
      int args[1] = {order};
      say("| physmem: alloc_order failed order=%d reason=out_of_physical_pages\n",
        args);
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

  if (physmem_sync_initialized) blocking_lock_release(&physmem_lock);

  assert_always(
    physmem_is_frame_address((unsigned)node),
    "physmem alloc: free list returned an invalid frame address.\n"
  );

  return node;
}

// Allocate a physical block of the requested order for a permanent owner.
void* physmem_leak_order(int order){
  void* page = physmem_alloc_order(order);
  assert_always(page != NULL,
    "physmem leak_order: boot-critical allocation exhausted physical memory.\n");
  __atomic_fetch_add(&order_leaks[order], 1);
  return page;
}

// free a physical page of given order
void physmem_free_order(void* page, int order){
  unsigned phys_addr = (unsigned)page;
  assert_always(page != NULL, "physmem free: page is NULL.\n");
  assert_always(
    physmem_is_frame_address(phys_addr),
    "physmem free: page is not a valid allocatable frame.\n"
  );

  assert(order >= 0 && order <= PHYS_FRAME_MAX_ORDER, "physmem free: invalid order.\n");
  assert((frame_index_from_address(phys_addr) & ((1u << order) - 1)) == 0, 
    "physmem free: page address is not aligned to its size.\n");

  if (physmem_sync_initialized) blocking_lock_acquire(&physmem_lock);

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

  if (physmem_sync_initialized) blocking_lock_release(&physmem_lock);
}

// allocate a physical page from core-local cache
void* physmem_alloc(void){
  enum CoreAffinity prev = core_pin();
  struct PerCore* per_core = get_per_core();

  // protect against re-entrance, needed because physmem_alloc_order can block
  if (physmem_sync_initialized) blocking_lock_acquire(&per_core->physmem_cache.lock);

  __atomic_fetch_add(&frames_alloced, 1);

  // refill cache if necessary, then pop and return a page
  if (per_core->physmem_cache.count == 0) {
    // Refill as many order-0 pages as remain. A partial refill is success; an
    // empty refill is ordinary exhaustion for this core.
    int filled = 0;
    for (int i = 0; i < LOCAL_CACHE_REFILL; i++) {
      void* page = physmem_alloc_order(0);
      if (page == NULL){
        break;
      }
      per_core->physmem_cache.pages[i] = page;
      filled++;
    }
    per_core->physmem_cache.count = (unsigned)filled;
    if (filled == 0){
      if (physmem_sync_initialized) blocking_lock_release(&per_core->physmem_cache.lock);
      core_unpin(prev);
      return NULL;
    }
  }

  // pop from local cache
  per_core->physmem_cache.count--;
  void* page = per_core->physmem_cache.pages[per_core->physmem_cache.count];

  if (physmem_sync_initialized) blocking_lock_release(&per_core->physmem_cache.lock);

  core_unpin(prev);

  return page;
}

// Allocate one permanent physical page for boot-lifetime state.
void* physmem_leak(void){
  void* page = physmem_alloc();
  assert_always(page != NULL,
    "physmem leak: boot-critical allocation exhausted physical memory.\n");
  __atomic_fetch_add(&frames_leaked, 1);
  return page;
}

// free a physical page
void physmem_free(void* page){
  unsigned phys_addr = (unsigned)page;
  assert_always(page != NULL, "physmem free: page is NULL.\n");
  assert_always(
    physmem_is_frame_address(phys_addr),
    "physmem free: page is not a valid order-0 allocatable frame.\n"
  );

  enum CoreAffinity prev = core_pin();
  struct PerCore* per_core = get_per_core();

  // protect against re-entrance
  if (physmem_sync_initialized) blocking_lock_acquire(&per_core->physmem_cache.lock);

  __atomic_fetch_add(&frames_freed, 1);

  // empty cache if full, then free to cache
  if (per_core->physmem_cache.count == LOCAL_CACHE_SIZE) {
    while (per_core->physmem_cache.count >= LOCAL_CACHE_REFILL){
      physmem_free_order(per_core->physmem_cache.pages[per_core->physmem_cache.count - 1], 0);
      per_core->physmem_cache.count--;
    }
  }

  per_core->physmem_cache.pages[per_core->physmem_cache.count] = page;
  per_core->physmem_cache.count++;

  if (physmem_sync_initialized) blocking_lock_release(&per_core->physmem_cache.lock);
  core_unpin(prev);
}

// Verify that every frame marked permanently allocated is accounted for.
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
