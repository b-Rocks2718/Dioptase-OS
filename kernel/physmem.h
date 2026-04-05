#ifndef PHYSMEM_H
#define PHYSMEM_H

#define FRAME_SIZE 4096
#define FRAMES_ADDR_START 0x800000
#define FRAMES_ADDR_END 0x7FBD000

#define PHYS_FRAME_COUNT 30653

// free pages store metadata to form a linked list
struct FreePageNode {
  struct FreePageNode *next;
};

// initialize physical page allocator
void physmem_init(void);

// allocate a physical page
// Panics if no free frames remain
void* physmem_alloc(void);

// free a physical page
void physmem_free(void* page);

#endif // PHYSMEM_H
