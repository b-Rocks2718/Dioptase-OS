#include "slab_heap.h"

#include "constants.h"
#include "physmem.h"
#include "debug.h"
#include "config.h"
#include "per_core.h"
#include "threads.h"

#define NUM_OBJECT_SIZES 9
unsigned OBJECT_SIZES[NUM_OBJECT_SIZES] = {4, 8, 16, 32, 64, 128, 256, 512, 1024};
struct SlabCache slab_caches[NUM_OBJECT_SIZES];

void slab_heap_init(){
  for (int i = 0; i < NUM_OBJECT_SIZES; i++) {
    blocking_lock_init(&slab_caches[i].lock);
    unsigned metadata_objects = (sizeof(struct Slab) + OBJECT_SIZES[i] - 1) / OBJECT_SIZES[i]; // Round up to nearest object size
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

struct Slab* slab_create(unsigned object_size) {
  struct Slab* slab = (struct Slab*)physmem_alloc();
  
  // work out how many objects the metadata takes up
  unsigned metadata_objects = (sizeof(struct Slab) + object_size - 1) / object_size; // Round up to nearest object size

  slab->free_list = (char*)slab + metadata_objects * object_size; // reserve objects for slab metadata
  slab->object_size = object_size;
  slab->free_objects = (FRAME_SIZE / object_size) - metadata_objects; // Calculate how many objects can fit in the slab
  slab->next = NULL;
  slab->prev = NULL;

  // Initialize the free list
  struct FreeObject* current = (struct FreeObject*)slab->free_list;
  for (unsigned i = 0; i < slab->free_objects - 1; i++) {
    current->next = (struct FreeObject*)((char*)current + object_size);
    current = current->next;
  }
  current->next = NULL; // Last object points to NULL

  return slab;
}

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

  blocking_lock_acquire(&cache->lock);

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

    blocking_lock_release(&cache->lock);
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

    blocking_lock_release(&cache->lock);
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

    blocking_lock_release(&cache->lock);
    return obj;
  }
}

void* slab_heap_alloc(unsigned size) {
  int i;
  bool valid_size = false;
  for (i = 0; i < NUM_OBJECT_SIZES; i++) {
    if (size <= OBJECT_SIZES[i]) {
      valid_size = true;
      break;
    }
  }

  assert(valid_size, "requested allocation size exceeds max supported size\n");

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

  preemption_restore(preempt_was);
  core_unpin(core_was);
  
  return obj;
}

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
  blocking_lock_acquire(&cache->lock);

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

  blocking_lock_release(&cache->lock);
}

void slab_heap_free(void* obj){
  // find slab containing obj
  struct Slab* slab = (struct Slab*)((unsigned)obj & ~(FRAME_SIZE - 1)); // Align down to slab boundary

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

      preemption_restore(preempt_was);
      core_unpin(core_was);
      return;
    }
  }
  panic("found slab with no matching cache\n");
}

void slab_heap_destroy() {
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

    blocking_lock_destroy(&cache->lock);
  }
}
