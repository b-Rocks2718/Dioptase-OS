#include "slab_heap.h"

#include "constants.h"
#include "physmem.h"
#include "debug.h"

#define NUM_OBJECT_SIZES 12
unsigned OBJECT_SIZES[NUM_OBJECT_SIZES] = {4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
struct SlabCache slab_caches[NUM_OBJECT_SIZES];

void slab_heap_init(){
  for (int i = 0; i < NUM_OBJECT_SIZES; i++) {
    blocking_lock_init(&slab_caches[i].lock);
    unsigned metadata_objects = (sizeof(struct Slab) + OBJECT_SIZES[i] - 1) / OBJECT_SIZES[i]; // Round up to nearest object size
    slab_caches[i].objects_per_slab = (FRAME_SIZE / OBJECT_SIZES[i]) - metadata_objects;
    slab_caches[i].full_slabs = NULL;
    slab_caches[i].partial_slabs = NULL;
    slab_caches[i].empty_slabs = NULL;
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
  for (unsigned i = metadata_objects; i < slab->free_objects - 1; i++) {
    current->next = (struct FreeObject*)((char*)current + object_size);
    current = current->next;
  }
  current->next = NULL; // Last object points to NULL

  return slab;
}

void* slab_heap_alloc(unsigned size){
  // Find the appropriate slab cache for the requested size
  struct SlabCache* cache = NULL;
  int i;
  for (i = 0; i < NUM_OBJECT_SIZES; i++) {
    if (size <= OBJECT_SIZES[i]) {
      cache = &slab_caches[i];
      break;
    }
  }
  
  if (cache == NULL) {
    return NULL; // No suitable cache found
  }

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

void slab_heap_free(void* obj){
  // find slab containing obj
  struct Slab* slab = (struct Slab*)((unsigned)obj & ~(FRAME_SIZE - 1)); // Align down to slab boundary

  // Find the appropriate slab cache for the object's size
  struct SlabCache* cache = NULL;
  for (int i = 0; i < NUM_OBJECT_SIZES; i++) {
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
    // Move slab from partial slabs list to empty slabs list
    
  } else if (0) {
    // Move slab from empty slabs list to partial slabs list
   
  }

  blocking_lock_release(&cache->lock);
}
