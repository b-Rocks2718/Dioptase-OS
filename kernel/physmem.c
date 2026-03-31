#include "physmem.h"
#include "print.h"
#include "debug.h"
#include "atomic.h"
#include "constants.h"

static struct SpinLock physmem_lock;
static struct FreePageNode* free_page_list = NULL;

// sanity check that something could be a frame address
static bool physmem_is_frame_address(unsigned phys_addr) {
  if (phys_addr < FRAMES_ADDR_START || phys_addr >= FRAMES_ADDR_END) {
    return false;
  }
  return (phys_addr & (FRAME_SIZE - 1)) == 0;
}

// initialize physical page allocator
void physmem_init(void){
  spin_lock_init(&physmem_lock);
  free_page_list = NULL;

  // Add each allocatable 4KiB frame exactly once. The free-list nodes live in
  // the pages themselves
  for (unsigned phys_addr = FRAMES_ADDR_START; 
      phys_addr < FRAMES_ADDR_END; 
      phys_addr += FRAME_SIZE) {
    physmem_free((void*)phys_addr);
  }
}

// allocate a physical page
void* physmem_alloc(void){
  spin_lock_acquire(&physmem_lock);

  if (!free_page_list) {
    spin_lock_release(&physmem_lock);
    panic("physmem alloc: out of physical pages.\n");
    return NULL;
  }

  struct FreePageNode* node = free_page_list;
  free_page_list = node->next;

  spin_lock_release(&physmem_lock);

  assert(
    physmem_is_frame_address((unsigned)node),
    "physmem alloc: free list returned an invalid frame address.\n"
  );

  return node;
}

// free a physical page
void physmem_free(void* page){
  unsigned phys_addr = (unsigned)page;
  assert(page != NULL, "physmem free: page is NULL.\n");
  assert(
    physmem_is_frame_address(phys_addr),
    "physmem free: page is not a valid allocatable frame.\n"
  );

  spin_lock_acquire(&physmem_lock);

  struct FreePageNode* node = (struct FreePageNode*)page;
  node->next = free_page_list;
  free_page_list = node;

  spin_lock_release(&physmem_lock);
}
