#ifndef PHYSMEM_H
#define PHYSMEM_H

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
