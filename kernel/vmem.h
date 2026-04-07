#ifndef VMEM_H
#define VMEM_H

#include "constants.h"
#include "ext.h"

struct VME {
  struct VME* next;

  unsigned start;
  unsigned end;

  unsigned size;
  bool shared;
  struct Node* file;
  unsigned file_offset;
};

// Initialize virtual memory structures
// Called once by the first core to set up global structures
void vmem_global_init(void);

// Per-core virtual memory initialization
void vmem_core_init(void);

// Map a file into memory, returning a pointer to the mapped region
void* mmap(unsigned size, bool shared, struct Node* file, unsigned file_offset);

// Unmap a previously mapped memory region
void munmap(void* p);

extern void tlb_kmiss_handler_(void);
extern void tlb_umiss_handler_(void);

#endif // VMEM_H