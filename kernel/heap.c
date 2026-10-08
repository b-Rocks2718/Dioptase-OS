#include "heap.h"

#include "constants.h"
#include "physmem.h"
#include "debug.h"
#include "config.h"
#include "per_core.h"
#include "threads.h"
#include "print.h"

#define HEAP_LARGE_ALLOC_NONE 0xFF // byte sentinel; live orders are 0..PHYS_FRAME_MAX_ORDER
unsigned OBJECT_SIZES[NUM_OBJECT_SIZES] = {4, 8, 16, 32, 64, 128, 256, 512, 1024};
struct SlabCache slab_caches[NUM_OBJECT_SIZES];
static bool heap_sync_initialized = false;

// Each cache keeps at most this many fully free slabs; further empty slabs
// return their frame to physmem.
#define MAX_EMPTY_SLABS 2

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

// Return whether an address is aligned and lies in the physical-frame heap.
static bool heap_is_frame_aligned_phys_addr(unsigned addr) {
  if (addr < FRAMES_ADDR_START || addr >= FRAMES_ADDR_END) {
    return false;
  }
  return (addr & (FRAME_SIZE - 1)) == 0;
}

// Acquire a heap lock once heap_sync_init() has run. Before then the kernel is
// single-threaded on core 0 and the locks do not exist yet; during teardown
// heap_destroy() turns locking off again before destroying them.
static void heap_lock(struct BlockingLock* lock) {
  if (heap_sync_initialized) blocking_lock_acquire(lock);
}

// Release a lock taken by heap_lock().
static void heap_unlock(struct BlockingLock* lock) {
  if (heap_sync_initialized) blocking_lock_release(lock);
}

// Return the buddy order needed to cover a large allocation.
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

// Record the buddy order in the header page of a large allocation.
static void large_allocation_mark(void* page, int order) {
  unsigned frame_index = frame_index_from_address((unsigned)page);

  heap_lock(&large_allocation_lock);

  assert_always(large_allocation_orders[frame_index] == HEAP_LARGE_ALLOC_NONE,
    "heap large alloc: physical frame is already tracked as a large allocation.\n");
  large_allocation_orders[frame_index] = order;

  heap_unlock(&large_allocation_lock);
}

// Allocate a physically contiguous large block, optionally leak-tracked.
static void* large_alloc(unsigned size, bool leaked) {
  int order = large_allocation_order_for_size(size);
  void* page = leaked ? physmem_leak_order(order) : physmem_alloc_order(order);
  if (page == NULL){
    return NULL;
  }

  large_allocation_mark(page, order);
  return page;
}

// Free a tracked large allocation and report whether it was recognized.
static bool large_free_if_tracked(void* obj) {
  unsigned addr = (unsigned)obj;
  if (!heap_is_frame_aligned_phys_addr(addr)) {
    return false;
  }

  unsigned frame_index = frame_index_from_address(addr);

  heap_lock(&large_allocation_lock);

  unsigned order = large_allocation_orders[frame_index];
  if (order == HEAP_LARGE_ALLOC_NONE) {
    heap_unlock(&large_allocation_lock);
    int args[1] = { (int)obj };
    say("heap free: frame-aligned pointer 0x%X is not a live large allocation\n", args);
    panic("heap free: invalid frame-aligned heap pointer.\n");
    return false;
  }

  large_allocation_orders[frame_index] = HEAP_LARGE_ALLOC_NONE;

  heap_unlock(&large_allocation_lock);

  physmem_free_order(obj, (int)order);
  return true;
}

// Initialize slab caches and the per-core large-allocation metadata.
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

// Initialize heap locks after all per-core heap state exists.
void heap_sync_init(){
  for (int i = 0; i < NUM_OBJECT_SIZES; i++) {
    blocking_lock_init(&slab_caches[i].lock);
  }
  blocking_lock_init(&large_allocation_lock);
  heap_sync_initialized = true;
}

// Return the smallest size class that fits size, or -1 if size needs a large
// physical-block allocation.
static int size_class_for(unsigned size) {
  for (int i = 0; i < NUM_OBJECT_SIZES; i++) {
    if (size <= OBJECT_SIZES[i]) {
      return i;
    }
  }
  return -1;
}

// Return the slab containing a slab-heap object. Slabs are single frames and
// keep their header at the frame base.
static struct Slab* slab_of(void* obj) {
  return (struct Slab*)((unsigned)obj & ~(FRAME_SIZE - 1));
}

// Return the size class serving slab. The header's object_size is immutable
// after creation, and the caller's live object keeps the slab allocated, so no
// lock is needed. A size matching no class means obj was never a slab object
// (or the header is corrupt), which is fatal.
static int size_class_of_slab(struct Slab* slab, void* obj) {
  for (int i = 0; i < NUM_OBJECT_SIZES; i++) {
    if (slab->object_size == OBJECT_SIZES[i]) {
      return i;
    }
  }

  int args[4] = {(int)obj, (int)slab, (int)slab->object_size, (int)slab->free_objects};
  say("| heap: obj=0x%X slab=0x%X object_size=%d free_objects=%d has no matching slab cache\n", args);
  panic("found slab with no matching cache\n");
  return -1;
}

// Push slab onto the front of one of a cache's doubly linked slab lists.
// Caller holds the cache lock.
static void slab_list_push(struct Slab** head, struct Slab* slab) {
  slab->prev = NULL;
  slab->next = *head;
  if (*head != NULL) {
    (*head)->prev = slab;
  }
  *head = slab;
}

// Unlink slab from the list headed by *head. Caller holds the cache lock.
static void slab_list_remove(struct Slab** head, struct Slab* slab) {
  if (slab->prev != NULL) {
    slab->prev->next = slab->next;
  } else {
    *head = slab->next;
  }
  if (slab->next != NULL) {
    slab->next->prev = slab->prev;
  }
  slab->next = NULL;
  slab->prev = NULL;
}

// Write the use-after-free poison over every word of a free object except the
// first, which holds the free-list link.
#ifdef HEAP_DEBUG
static void heap_poison_object(void* obj, unsigned object_size) {
  for (unsigned i = 1; i < object_size / 4; i++) {
    ((unsigned*)obj)[i] = HEAP_POISON;
  }
}
#endif

// Allocate and initialize one slab for size class class_index, threading every
// object after the reserved header objects onto the slab's free list.
// Returns NULL if no physical frame is available.
static struct Slab* slab_create(int class_index) {
  struct Slab* slab = (struct Slab*)physmem_alloc();
  if (slab == NULL){
    return NULL;
  }

  unsigned object_size = OBJECT_SIZES[class_index];
  unsigned objects_per_slab = slab_caches[class_index].objects_per_slab;
  unsigned metadata_objects = (FRAME_SIZE / object_size) - objects_per_slab;

  slab->free_list = (char*)slab + metadata_objects * object_size; // reserve objects for slab metadata
  slab->object_size = object_size;
  slab->free_objects = objects_per_slab;
  slab->next = NULL;
  slab->prev = NULL;

  struct FreeObject* current = (struct FreeObject*)slab->free_list;
  for (unsigned i = 0; i < objects_per_slab; i++) {
    #ifdef HEAP_DEBUG
    heap_poison_object(current, object_size);
    #endif
    bool last = (i + 1 == objects_per_slab);
    current->next = last ? NULL : (struct FreeObject*)((char*)current + object_size);
    current = current->next;
  }

  #ifdef HEAP_DEBUG
  // initialize allocation bitmap to all 0's (all free)
  unsigned bitmap_size = (objects_per_slab + 7) / 8; // Round up to nearest byte
  for (unsigned j = 0; j < bitmap_size; j++) {
    slab->allocation_bitmap[j] = 0;
  }
  #endif

  return slab;
}

#ifdef HEAP_DEBUG
// Set (allocate=true) or clear one object's bit in its slab's debug bitmap,
// panicking on a double allocation or double free.
static void bitmap_update(struct Slab* slab, void* obj, bool allocate) {
  struct SlabCache* cache = &slab_caches[size_class_of_slab(slab, obj)];

  unsigned metadata_end = (unsigned)slab + (FRAME_SIZE - cache->objects_per_slab * slab->object_size);
  unsigned obj_index = ((unsigned)obj - metadata_end) / slab->object_size;
  unsigned byte_index = obj_index / 8;
  char bit = (char)(1u << (obj_index % 8));

  heap_lock(&cache->lock);
  bool was_allocated = (slab->allocation_bitmap[byte_index] & bit) != 0;
  if (allocate) {
    assert_always(!was_allocated, "double allocation detected in slab allocator\n");
    slab->allocation_bitmap[byte_index] |= bit;
  } else {
    assert_always(was_allocated, "double free detected in slab allocator\n");
    slab->allocation_bitmap[byte_index] &= ~bit;
  }
  heap_unlock(&cache->lock);
}
#endif

// Take one object of size class class_index from its shared slab cache,
// preferring partial slabs, then a cached empty slab, then a new slab.
// Caller holds cache->lock. Returns NULL only when a new slab is needed and
// physmem is exhausted.
static void* slab_alloc_locked(struct SlabCache* cache, int class_index){
  struct Slab* slab = cache->partial_slabs;
  if (slab == NULL) {
    slab = cache->empty_slabs;
    if (slab != NULL) {
      slab_list_remove(&cache->empty_slabs, slab);
      cache->num_empty_slabs--;
    } else {
      slab = slab_create(class_index);
      if (slab == NULL) {
        return NULL;
      }
    }
    slab_list_push(&cache->partial_slabs, slab);
  }

  void* obj = slab->free_list;
  slab->free_list = ((struct FreeObject*)obj)->next;
  slab->free_objects--;

  if (slab->free_objects == 0) {
    slab_list_remove(&cache->partial_slabs, slab);
    slab_list_push(&cache->full_slabs, slab);
  }

  return obj;
}

// Take up to `want` objects of size class class_index from the shared cache
// under a single cache-lock hold, returned as a FreeObject chain whose last
// node is stored in *tail. Fewer than `want` (possibly zero) are returned only
// when physmem is exhausted.
//
// Refilling a per-core free list one locked object at a time made a cold
// refill (e.g. the first allocation after a thread migrates to another core)
// cost tens of thousands of cycles per size class. One lock hold amortizes the
// lock and preemption overhead across the whole batch.
static unsigned slab_alloc_batch(int class_index, unsigned want,
    struct FreeObject** head, struct FreeObject** tail){
  struct SlabCache* cache = &slab_caches[class_index];
  struct FreeObject* chain = NULL;
  struct FreeObject* last = NULL;
  unsigned got = 0;

  heap_lock(&cache->lock);
  while (got < want) {
    struct FreeObject* obj = (struct FreeObject*)slab_alloc_locked(cache, class_index);
    if (obj == NULL) {
      break;
    }
    obj->next = chain;
    chain = obj;
    if (last == NULL) {
      last = obj;
    }
    got++;
  }
  heap_unlock(&cache->lock);

  *head = chain;
  *tail = last;
  return got;
}

// Return one object to its slab, moving the slab between the full, partial,
// and empty lists as its free count changes. Caller holds cache->lock, and
// cache must be the cache serving obj's size class.
static void slab_free_locked(struct SlabCache* cache, void* obj) {
  struct Slab* slab = slab_of(obj);

  ((struct FreeObject*)obj)->next = slab->free_list;
  slab->free_list = obj;
  slab->free_objects++;

  if (slab->free_objects == 1) {
    // The slab was full.
    slab_list_remove(&cache->full_slabs, slab);
    slab_list_push(&cache->partial_slabs, slab);
  }

  if (slab->free_objects == cache->objects_per_slab) {
    slab_list_remove(&cache->partial_slabs, slab);
    if (cache->num_empty_slabs >= MAX_EMPTY_SLABS) {
      physmem_free(slab);
    } else {
      slab_list_push(&cache->empty_slabs, slab);
      cache->num_empty_slabs++;
    }
  }
}

// Return a NULL-terminated chain of size-class class_index objects to the
// shared cache under a single cache-lock hold (see slab_alloc_batch()).
static void slab_free_batch(int class_index, struct FreeObject* chain) {
  struct SlabCache* cache = &slab_caches[class_index];
  heap_lock(&cache->lock);
  while (chain != NULL) {
    struct FreeObject* next = chain->next;
    slab_free_locked(cache, chain);
    chain = next;
  }
  heap_unlock(&cache->lock);
}

// Allocate one object of size class class_index from this core's free list,
// refilling the list from the shared slab cache when it runs low.
//
// The current thread stays pinned so `core` remains this core's PerCore, and
// preemption is disabled whenever the per-core list is touched so another
// thread on this core cannot interleave. Preemption is re-enabled around
// slab_alloc_batch() because it may block on the cache lock or physmem; the
// batch is spliced in afterwards, so a list that another thread on this core
// refilled meanwhile may briefly exceed the refill target (free() trims it).
// Returns NULL only if both the per-core list and the slab cache are empty.
static void* per_core_alloc(int class_index) {
  enum CoreAffinity core_was = core_pin();
  struct PerCore* core = get_per_core();

  bool preempt_was = preemption_disable();
  unsigned size = core->free_list_sizes[class_index];
  if (size <= MIN_PER_CORE_FREE_LIST) {
    preemption_restore(preempt_was);
    struct FreeObject* head;
    struct FreeObject* tail;
    unsigned got = slab_alloc_batch(class_index,
      PER_CORE_FREE_LIST_REFILL + 1 - size, &head, &tail);
    preempt_was = preemption_disable();

    // On exhaustion `got` may be short or zero; use whatever we got.
    if (got > 0) {
      tail->next = core->free_lists[class_index];
      core->free_lists[class_index] = head;
      core->free_list_sizes[class_index] += got;
    }
  }

  void* obj = core->free_lists[class_index];
  if (obj != NULL) {
    core->free_lists[class_index] = ((struct FreeObject*)obj)->next;
    core->free_list_sizes[class_index]--;

    #ifdef HEAP_DEBUG
    bitmap_update(slab_of(obj), obj, true);
    // check the poison value is still there
    for (unsigned i = 1; i < OBJECT_SIZES[class_index] / 4; i++) {
      assert_always(((unsigned*)obj)[i] == HEAP_POISON,
        "heap corruption detected: likely use after free\n");
    }
    #endif
  }

  preemption_restore(preempt_was);
  core_unpin(core_was);
  return obj;
}

// Allocate size bytes from the slab heap or as a large physical block.
// Kernel allocations do not fail: exhausting physical memory panics.
static void* alloc(unsigned size, bool leaked) {
  assert(size > 0, "heap alloc: requested a zero-byte allocation.\n");

  int class_index = size_class_for(size);
  void* obj = (class_index < 0) ? large_alloc(size, leaked)
                                : per_core_alloc(class_index);

  if (obj == NULL) {
    int args[1] = {(int)size};
    say("| heap: out of memory allocating %u bytes\n", args);
    panic("heap alloc: kernel allocation failed because physical memory is exhausted.\n");
  }
  return obj;
}

// Allocate kernel memory that may later be returned with free().
void* malloc(unsigned size) {
  #ifdef HEAP_DEBUG
  __atomic_fetch_add((int*)&n_malloc, 1);
  #endif
  return alloc(size, false);
}

// Allocate kernel memory whose lifetime extends until kernel shutdown.
void* leak(unsigned size) {
  #ifdef HEAP_DEBUG
  __atomic_fetch_add((int*)&n_leak, 1);
  #endif
  return alloc(size, true);
}

// Check whether a pointer could be a slab heap object at all.
static bool slab_free_sanity(void* obj){
  // check obj is within slab heap bounds
  if ((unsigned)obj < (unsigned)FRAMES_ADDR_START ||
      (unsigned)obj >= (unsigned)FRAMES_ADDR_END) {
    return false;
  }

  // check obj is aligned to smallest object size
  return (unsigned)obj % OBJECT_SIZES[0] == 0;
}

// Free a reclaimable allocation and reject invalid or duplicate frees.
void free(void* obj){
  // Like C's free(), releasing NULL is a no-op so cleanup paths need no guard.
  if (obj == NULL) {
    return;
  }

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

  struct Slab* slab = slab_of(obj);
  int class_index = size_class_of_slab(slab, obj);

  #ifdef HEAP_DEBUG
  if ((unsigned)obj % slab->object_size != 0) {
    say("invalid pointer passed to slab_free: %X\n", &obj);
    panic("attempting to free pointer that is not aligned to its object size\n");
  }
  #endif

  // Same pinning/preemption discipline as per_core_alloc().
  enum CoreAffinity core_was = core_pin();
  bool preempt_was = preemption_disable();
  struct PerCore* core = get_per_core();

  if (core->free_list_sizes[class_index] >= MAX_PER_CORE_FREE_LIST) {
    // Per-core free list is full: detach the excess while preemption is off,
    // then return it to the shared slab cache in one locked batch.
    struct FreeObject* spill = NULL;
    while (core->free_list_sizes[class_index] > PER_CORE_FREE_LIST_REFILL) {
      struct FreeObject* victim = core->free_lists[class_index];
      core->free_lists[class_index] = victim->next;
      core->free_list_sizes[class_index]--;
      victim->next = spill;
      spill = victim;
    }

    preemption_restore(preempt_was);
    slab_free_batch(class_index, spill);
    preempt_was = preemption_disable();
  }

  ((struct FreeObject*)obj)->next = core->free_lists[class_index];
  core->free_lists[class_index] = obj;
  core->free_list_sizes[class_index]++;

  #ifdef HEAP_DEBUG
  bitmap_update(slab, obj, false);
  heap_poison_object(obj, slab->object_size);
  #endif

  preemption_restore(preempt_was);
  core_unpin(core_was);
}

// Tear down heap synchronization after all reclaimable allocations are gone.
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

    struct Slab* lists[3] = {cache->full_slabs, cache->partial_slabs, cache->empty_slabs};
    for (int list = 0; list < 3; list++) {
      struct Slab* slab = lists[list];
      while (slab != NULL) {
        struct Slab* next = slab->next;
        physmem_free(slab);
        slab = next;
      }
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
