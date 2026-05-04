#include "heap.h"

#include "constants.h"
#include "physmem.h"
#include "debug.h"
#include "config.h"
#include "per_core.h"
#include "threads.h"
#include "print.h"

#define NUM_OBJECT_SIZES 9
#define HEAP_LARGE_ALLOC_NONE 0xFF // byte sentinel; live orders are 0..PHYS_FRAME_MAX_ORDER
unsigned OBJECT_SIZES[NUM_OBJECT_SIZES] = {4, 8, 16, 32, 64, 128, 256, 512, 1024};
struct SlabCache slab_caches[NUM_OBJECT_SIZES];
static bool heap_sync_initialized = false;

/*
 * Large allocation side table.
 *
 * Slabs only manage objects up to OBJECT_SIZES[NUM_OBJECT_SIZES - 1]. Larger
 * heap requests are backed by whole physmem blocks and return the block base
 * directly. The first frame index of the block stores the order so free() can
 * route the exact pointer back to physmem_free_order().
 *
 * Invariants:
 * - HEAP_LARGE_ALLOC_NONE means the frame is not the first frame of a live
 *   large heap allocation.
 * - A non-NONE entry is present only at the first frame of an allocation.
 * - Live order values fit in one byte because PHYS_FRAME_MAX_ORDER is 14.
 * - Large allocations must be freed with the exact pointer returned by malloc().
 */
static unsigned char large_allocation_orders[PHYS_FRAME_COUNT];
static struct BlockingLock large_allocation_lock;

#ifdef HEAP_DEBUG
unsigned n_malloc = 0;
unsigned n_free = 0;
unsigned n_leak = 0;
#endif

static bool heap_is_frame_aligned_phys_addr(unsigned addr) {
  if (addr < FRAMES_ADDR_START || addr >= FRAMES_ADDR_END) {
    return false;
  }
  return (addr & (FRAME_SIZE - 1)) == 0;
}

static int large_allocation_order_for_size(unsigned size) {
  int order = 0;
  unsigned block_size = FRAME_SIZE;

  while (block_size < size) {
    assert(order < PHYS_FRAME_MAX_ORDER,
      "heap large alloc: requested allocation exceeds max physical block size.\n");
    order++;
    block_size = block_size * 2;
  }

  return order;
}

static void large_allocation_mark(void* page, int order) {
  unsigned frame_index = frame_index_from_address((unsigned)page);

  if (heap_sync_initialized) blocking_lock_acquire(&large_allocation_lock);

  assert(large_allocation_orders[frame_index] == HEAP_LARGE_ALLOC_NONE,
    "heap large alloc: physical frame is already tracked as a large allocation.\n");
  large_allocation_orders[frame_index] = order;

  if (heap_sync_initialized) blocking_lock_release(&large_allocation_lock);
}

static void* large_alloc(unsigned size, bool leaked) {
  int order = large_allocation_order_for_size(size);
  void* page = leaked ? physmem_leak_order(order) : physmem_alloc_order(order);

  large_allocation_mark(page, order);
  return page;
}

static bool large_free_if_tracked(void* obj) {
  unsigned addr = (unsigned)obj;
  if (!heap_is_frame_aligned_phys_addr(addr)) {
    return false;
  }

  unsigned frame_index = frame_index_from_address(addr);

  if (heap_sync_initialized) blocking_lock_acquire(&large_allocation_lock);

  unsigned order = large_allocation_orders[frame_index];
  if (order == HEAP_LARGE_ALLOC_NONE) {
    if (heap_sync_initialized) blocking_lock_release(&large_allocation_lock);
    return false;
  }

  large_allocation_orders[frame_index] = HEAP_LARGE_ALLOC_NONE;

  if (heap_sync_initialized) blocking_lock_release(&large_allocation_lock);

  physmem_free_order(obj, (int)order);
  return true;
}

void heap_init(){
  heap_large_alloc_init(large_allocation_orders,
    PHYS_FRAME_COUNT, HEAP_LARGE_ALLOC_NONE);

  for (int i = 0; i < NUM_OBJECT_SIZES; i++) {
    unsigned slab_size = sizeof(struct Slab);
    #ifdef HEAP_DEBUG
    // remove bitmap from size, we calculate the real size here
    slab_size -= 4;

    // add in bitmap size
    slab_size += (FRAME_SIZE - (sizeof(struct Slab) - 4) + 8 * OBJECT_SIZES[i]) / (8 * OBJECT_SIZES[i] + 1);
    #endif
    unsigned metadata_objects = (slab_size + OBJECT_SIZES[i] - 1) / OBJECT_SIZES[i]; // Round up to nearest object size
    slab_caches[i].objects_per_slab = (FRAME_SIZE / OBJECT_SIZES[i]) - metadata_objects;
    slab_caches[i].full_slabs = NULL;
    slab_caches[i].partial_slabs = NULL;
    slab_caches[i].empty_slabs = NULL;
    slab_caches[i].num_empty_slabs = 0;
  }

  for (int i = 0; i < MAX_CORES; i++) {
    for (int j = 0; j < NUM_OBJECT_SIZES; j++) {
      per_core_data[i].free_lists[j] = NULL;
      per_core_data[i].free_list_sizes[j] = 0;
    }
  }
}

void heap_sync_init(){
  for (int i = 0; i < NUM_OBJECT_SIZES; i++) {
    blocking_lock_init(&slab_caches[i].lock);
  }
  blocking_lock_init(&large_allocation_lock);
  heap_sync_initialized = true;
}

struct Slab* slab_create(unsigned object_size) {
  struct Slab* slab = (struct Slab*)physmem_alloc();

  int i;
  for (i = 0; i < NUM_OBJECT_SIZES; i++) {
    if (object_size <= OBJECT_SIZES[i]) {
      break;
    }
  }
  assert(i < NUM_OBJECT_SIZES, "slab_create: object size exceeds max supported size\n");

  unsigned metadata_objects = (FRAME_SIZE / object_size) - slab_caches[i].objects_per_slab;

  slab->free_list = (char*)slab + metadata_objects * object_size; // reserve objects for slab metadata
  slab->object_size = object_size;
  slab->free_objects = (FRAME_SIZE / object_size) - metadata_objects; // Calculate how many objects can fit in the slab
  slab->next = NULL;
  slab->prev = NULL;

  // Initialize the free list
  struct FreeObject* current = (struct FreeObject*)slab->free_list;
  for (unsigned i = 0; i < slab->free_objects - 1; i++) {
    #ifdef HEAP_DEBUG
    // poison free objects in debug mode to make use-after-free more obvious
    for (int i = 1; i < object_size / 4; i++) {
      ((unsigned*)current)[i] = HEAP_POISON;
    }
    #endif

    current->next = (struct FreeObject*)((char*)current + object_size);
    current = current->next;
  }
  current->next = NULL; // Last object points to NULL

  #ifdef HEAP_DEBUG
  // poison final object
  for (int i = 1; i < object_size / 4; i++) {
    ((unsigned*)current)[i] = HEAP_POISON;
  }
  #endif

  #ifdef HEAP_DEBUG
  // initialize allocation bitmap to all 0's (all free)
  unsigned bitmap_size = (slab_caches[i].objects_per_slab + 7) / 8; // Round up to nearest byte
  for (unsigned j = 0; j < bitmap_size; j++) {
    slab->allocation_bitmap[j] = 0;
  }
  #endif

  return slab;
}

#ifdef HEAP_DEBUG
void bitmap_alloc(struct Slab* slab, void* obj) {
  // find slab cache for this slab
  struct SlabCache* cache = NULL;
  for (int i = 0; i < NUM_OBJECT_SIZES; i++) {
    if (slab->object_size == OBJECT_SIZES[i]) {
      cache = &slab_caches[i];
      break;
    }
  }
  if (cache == NULL) {
    int args[4] = {(int)obj, (int)slab, (int)slab->object_size, (int)slab->free_objects};
    say("heap bitmap_alloc: obj=0x%X slab=0x%X object_size=%d free_objects=%d\n", args);
    panic("heap bitmap_alloc: object is not in a valid slab cache\n");
  }

  unsigned metadata_end = (unsigned)slab + (FRAME_SIZE - cache->objects_per_slab * slab->object_size);
  unsigned obj_index = ((unsigned)obj - metadata_end) / slab->object_size;
  unsigned byte_index = obj_index / 8;
  unsigned bit_index = obj_index % 8;

  if (heap_sync_initialized) blocking_lock_acquire(&cache->lock);

  // assert that the bit is not already set (double allocation)
  assert((slab->allocation_bitmap[byte_index] & (1u << bit_index)) == 0,
    "double allocation detected in slab allocator\n");

  // mark as allocated in bitmap
  slab->allocation_bitmap[byte_index] |= (1u << bit_index);

  if (heap_sync_initialized) blocking_lock_release(&cache->lock);
}
#endif

void* slab_alloc(unsigned size){
  // Find the appropriate slab cache for the requested size
  struct SlabCache* cache = NULL;
  int i;
  for (i = 0; i < NUM_OBJECT_SIZES; i++) {
    if (size <= OBJECT_SIZES[i]) {
      cache = &slab_caches[i];
      break;
    }
  }

  assert(cache != NULL, "requested allocation size exceeds max supported size\n");

  if (heap_sync_initialized) blocking_lock_acquire(&cache->lock);

  if (cache->partial_slabs != NULL) {
    // Allocate from a partial slab
    struct Slab* slab = cache->partial_slabs;
    void* obj = slab->free_list;
    slab->free_list = ((struct FreeObject*)obj)->next; // Update free list
    slab->free_objects--;

    if (slab->free_objects == 0) {
      // Move slab from partial slabs list to full slabs list

      // Remove slab from partial slabs list
      cache->partial_slabs = slab->next;
      if (cache->partial_slabs != NULL) {
        cache->partial_slabs->prev = NULL;
      }

      // Add slab to full slabs list
      slab->next = cache->full_slabs;
      if (cache->full_slabs != NULL) {
        cache->full_slabs->prev = slab;
      }
      slab->prev = NULL;
      cache->full_slabs = slab;
    }

    if (heap_sync_initialized) blocking_lock_release(&cache->lock);
    return obj;
  } else if (cache->empty_slabs != NULL) {
    // Move an empty slab to the partial slabs list

    // remove slab from empty slabs list
    struct Slab* slab = cache->empty_slabs;
    cache->empty_slabs = slab->next;
    if (cache->empty_slabs != NULL) {
      cache->empty_slabs->prev = NULL;
    }

    // add slab to partial slabs list
    slab->next = cache->partial_slabs;
    if (cache->partial_slabs != NULL) {
      cache->partial_slabs->prev = slab;
    }
    slab->prev = NULL;
    cache->partial_slabs = slab;
    cache->num_empty_slabs--;

    // Allocate from the newly moved slab
    void* obj = slab->free_list;
    slab->free_list = ((struct FreeObject*)obj)->next; // Update free list
    slab->free_objects--;

    if (heap_sync_initialized) blocking_lock_release(&cache->lock);
    return obj;
  } else {
    // No available slabs, create a new one
    struct Slab* new_slab = slab_create(OBJECT_SIZES[i]);

    // Add the new slab to the partial slabs list
    if (cache->partial_slabs != NULL) {
      cache->partial_slabs->prev = new_slab;
    }
    new_slab->next = cache->partial_slabs;
    new_slab->prev = NULL;
    cache->partial_slabs = new_slab;

    // Allocate from the new slab
    void* obj = new_slab->free_list;
    new_slab->free_list = ((struct FreeObject*)obj)->next; // Update free list
    new_slab->free_objects--;

    if (heap_sync_initialized) blocking_lock_release(&cache->lock);
    return obj;
  }
}

static void* alloc(unsigned size, bool leaked) {
  assert(size > 0, "tried to alloc 0 bytes?\n");

  int i;
  bool valid_size = false;
  for (i = 0; i < NUM_OBJECT_SIZES; i++) {
    if (size <= OBJECT_SIZES[i]) {
      valid_size = true;
      break;
    }
  }

  if (!valid_size) {
    return large_alloc(size, leaked);
  }

  int core_was = core_pin();
  struct PerCore* core = get_per_core();

  // need to disable preemption around modification to per-core free list
  int preempt_was = preemption_disable();
  if (core->free_list_sizes[i] <= MIN_PER_CORE_FREE_LIST) {
    // per-core free list is empty, we should get some objects from the slab cache
    while (core->free_list_sizes[i] <= PER_CORE_FREE_LIST_REFILL) {
      preemption_restore(preempt_was);

      // enable preemption around blocking call, but keep pinned to this core
      void* obj = slab_alloc(OBJECT_SIZES[i]);
      if (obj == NULL) {
        preempt_was = preemption_disable(); // to match expected state at the end of the loop
        break; // slab cache is out of memory, we have to return what we got so far
      }

      preempt_was = preemption_disable();
      // Free to per-core free list
      ((struct FreeObject*)obj)->next = core->free_lists[i];
      core->free_lists[i] = obj;
      core->free_list_sizes[i]++;
    }
  }

  // alloc from per-core free list
  void* obj = core->free_lists[i];
  if (obj != NULL) {
    core->free_lists[i] = ((struct FreeObject*)obj)->next; // Update free list
    core->free_list_sizes[i]--;
  }

  #ifdef HEAP_DEBUG
  struct Slab* slab = (struct Slab*)((unsigned)obj & ~(FRAME_SIZE - 1)); // Align down to slab boundary

  // update allocation bitmap
  bitmap_alloc(slab, obj);

  // check the poison value is still there
  for (int i = 1; i < slab->object_size / 4; i++) {
    assert(((unsigned*)obj)[i] == HEAP_POISON,
      "heap corruption detected: likely use after free\n");
  }
  #endif

  preemption_restore(preempt_was);
  core_unpin(core_was);

  return obj;
}

void* malloc(unsigned size) {
  #ifdef HEAP_DEBUG
  __atomic_fetch_add((int*)&n_malloc, 1);
  #endif
  return alloc(size, false);
}

void* leak(unsigned size) {
  #ifdef HEAP_DEBUG
  __atomic_fetch_add((int*)&n_leak, 1);
  #endif
  return alloc(size, true);
}

#ifdef HEAP_DEBUG
void bitmap_free(struct Slab* slab, void* obj) {
  struct SlabCache* cache = NULL;
  for (int i = 0; i < NUM_OBJECT_SIZES; i++) {
    if (slab->object_size == OBJECT_SIZES[i]) {
      cache = &slab_caches[i];
      break;
    }
  }
  assert(cache != NULL, "found slab with no matching cache\n");

  unsigned metadata_end = (unsigned)slab + (FRAME_SIZE - cache->objects_per_slab * slab->object_size);
  unsigned obj_index = ((unsigned)obj - metadata_end) / slab->object_size;
  unsigned byte_index = obj_index / 8;
  unsigned bit_index = obj_index % 8;

  if (heap_sync_initialized) blocking_lock_acquire(&cache->lock);

  // assert that the bit is currently set (double free)
  assert((slab->allocation_bitmap[byte_index] & (1u << bit_index)) != 0,
    "double free detected in slab allocator\n");

  // mark as free in bitmap
  slab->allocation_bitmap[byte_index] &= ~(1u << bit_index);
  if (heap_sync_initialized) blocking_lock_release(&cache->lock);
}
#endif

void slab_free(void* obj) {
  struct Slab* slab = (struct Slab*)((unsigned)obj & ~(FRAME_SIZE - 1)); // Align down to slab boundary

  // Find the appropriate slab cache for the object's size
  struct SlabCache* cache = NULL;
  for (int i = 0; i < NUM_OBJECT_SIZES; i++) {
    // we don't need to lock the slab or anything to read object size, because
    // the slab metadata is immutable after creation
    // and the slab won't be destroyed because we know at least one object
    // has not yet been freed back to it (the one we're freeing now)
    if (slab->object_size == OBJECT_SIZES[i]) {
      cache = &slab_caches[i];
      break;
    }
  }

  assert(cache != NULL, "found slab with no matching cache\n");
  if (heap_sync_initialized) blocking_lock_acquire(&cache->lock);

  // Free the object back to the slab
  ((struct FreeObject*)obj)->next = slab->free_list;
  slab->free_list = obj;
  slab->free_objects++;

  if (slab->free_objects == cache->objects_per_slab) {
    // Remove slab from partial slabs list
    if (slab->prev != NULL) {
      slab->prev->next = slab->next;
    } else {
      cache->partial_slabs = slab->next;
    }
    if (slab->next != NULL) {
      slab->next->prev = slab->prev;
    }

    if (cache->num_empty_slabs >= 2) {
      // If we already have 2 empty slabs, free this one back to physical memory
      physmem_free(slab);
    } else {
      // Move slab from partial slabs list to empty slabs list

      // Add slab to empty slabs list
      slab->next = cache->empty_slabs;
      if (cache->empty_slabs != NULL) {
        cache->empty_slabs->prev = slab;
      }
      slab->prev = NULL;
      cache->empty_slabs = slab;
      cache->num_empty_slabs++;
    }
  } else if (slab->free_objects == 1) {
    // Move slab from full slabs list to partial slabs list

    // remove slab from full slabs list
    if (slab->prev != NULL) {
      slab->prev->next = slab->next;
    } else {
      cache->full_slabs = slab->next;
    }
    if (slab->next != NULL) {
      slab->next->prev = slab->prev;
    }

    // add slab to partial slabs list
    slab->next = cache->partial_slabs;
    if (cache->partial_slabs != NULL) {
      cache->partial_slabs->prev = slab;
    }
    slab->prev = NULL;
    cache->partial_slabs = slab;
  }

  if (heap_sync_initialized) blocking_lock_release(&cache->lock);
}

bool slab_free_sanity(void* obj){
  // check obj is within slab heap bounds
  if ((unsigned)obj < (unsigned)FRAMES_ADDR_START ||
      (unsigned)obj >= (unsigned)FRAMES_ADDR_END) {
    return false;
  }

  // check obj is aligned to smallest object size
  if ((unsigned)obj % OBJECT_SIZES[0] != 0) {
    return false;
  }

  return true;
}

void free(void* obj){
  #ifdef HEAP_DEBUG
  __atomic_fetch_add((int*)&n_free, 1);

  if (!slab_free_sanity(obj)) {
    say("invalid pointer passed to slab_free: %X\n", &obj);
    panic("attempting to free invalid pointer\n");
  }
  #endif

  if (large_free_if_tracked(obj)) {
    return;
  }

  // find slab containing obj
  struct Slab* slab = (struct Slab*)((unsigned)obj & ~(FRAME_SIZE - 1)); // Align down to slab boundary

  #ifdef HEAP_DEBUG
  // check object size makes sense
  bool valid_size = false;
  for (int i = 0; i < NUM_OBJECT_SIZES; i++) {
    if (slab->object_size == OBJECT_SIZES[i]) {
      valid_size = true;
      break;
    }
  }
  if (!valid_size) {
    say("invalid pointer passed to slab_free: %X\n", &obj);
    panic("attempting to free pointer with invalid object size\n");
  }

  if ((unsigned)obj % slab->object_size != 0) {
    say("invalid pointer passed to slab_free: %X\n", &obj);
    panic("attempting to free pointer that is not aligned to its object size\n");
  }
  #endif

  int core_was = core_pin();
  int preempt_was = preemption_disable();

  struct PerCore* core = get_per_core();
  for (int i = 0; i < NUM_OBJECT_SIZES; i++) {
    if (slab->object_size == OBJECT_SIZES[i]) {
      if (core->free_list_sizes[i] >= MAX_PER_CORE_FREE_LIST) {
        // per-core free list is full, we should move some back to the slab cache
        while (core->free_list_sizes[i] > PER_CORE_FREE_LIST_REFILL) {
          void* free_obj = core->free_lists[i];
          core->free_lists[i] = ((struct FreeObject*)free_obj)->next; // Update free list
          core->free_list_sizes[i]--;

          preemption_restore(preempt_was);
          // enable preemption around blocking call, but keep pinned to this core

          // Free back to slab cache
          slab_free(free_obj);

          preempt_was = preemption_disable();
        }
      }

      // Free to per-core free list
      ((struct FreeObject*)obj)->next = core->free_lists[i];
      core->free_lists[i] = obj;
      core->free_list_sizes[i]++;

      #ifdef HEAP_DEBUG
      bitmap_free(slab, obj);

      // write poison value to freed object to make use-after-free more obvious
      for (int i = 1; i < slab->object_size / 4; i++) {
        ((unsigned*)obj)[i] = HEAP_POISON;
      }
      #endif

      preemption_restore(preempt_was);
      core_unpin(core_was);
      return;
    }
  }
  {
    int args[4] = {(int)obj, (int)slab, (int)slab->object_size, (int)slab->free_objects};
    say("free no cache: obj=0x%X slab=0x%X object_size=%d free_objects=%d\n", args);
    panic("found slab with no matching cache\n");
  }
}

void heap_destroy() {
  bool locks_initialized = heap_sync_initialized;

  // kernel_shutdown() stops all other heap users before calling heap_destroy().
  // From this point on the cache locks are teardown objects, not synchronization
  // guards. Disable allocator locking before destroying those locks because each
  // lock owns a CLH tail node that was itself allocated from this heap.
  heap_sync_initialized = false;

  if (locks_initialized) {
    for (int i = 0; i < NUM_OBJECT_SIZES; i++) {
      blocking_lock_destroy(&slab_caches[i].lock);
    }
    blocking_lock_destroy(&large_allocation_lock);
  }

  for (int i = 0; i < NUM_OBJECT_SIZES; i++) {
    struct SlabCache* cache = &slab_caches[i];

    // Free full slabs
    struct Slab* slab = cache->full_slabs;
    while (slab != NULL) {
      struct Slab* next = slab->next;
      physmem_free(slab);
      slab = next;
    }

    // Free partial slabs
    slab = cache->partial_slabs;
    while (slab != NULL) {
      struct Slab* next = slab->next;
      physmem_free(slab);
      slab = next;
    }

    // Free empty slabs
    slab = cache->empty_slabs;
    while (slab != NULL) {
      struct Slab* next = slab->next;
      physmem_free(slab);
      slab = next;
    }
  }

  #ifdef HEAP_DEBUG
    int args[3] = {n_free, n_leak, n_malloc};
    if (n_free != n_malloc) {
      say("| Warning: heap malloc/free mismatch: n_free:%d n_leak:%d n_malloc:%d\n", args);
    } else {
      say("| No heap malloc leaks detected: n_free:%d n_leak:%d n_malloc:%d\n", args);
    }
  #endif
}
