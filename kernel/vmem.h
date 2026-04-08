#ifndef VMEM_H
#define VMEM_H

#include "constants.h"
#include "ext.h"

// flags to pass into mmap
#define MMAP_NONE   0x00
#define MMAP_SHARED 0x01
#define MMAP_READ   0x04
#define MMAP_WRITE  0x08
#define MMAP_EXEC   0x10

// tlb/pde/pte flags
#define VMEM_READ   0x01
#define VMEM_WRITE  0x02
#define VMEM_EXEC   0x04
#define VMEM_USER   0x08
#define VMEM_GLOBAL 0x10
#define VMEM_VALID  0x20
#define VMEM_DIRTY  0x40

// begin vmem allocations from 0x80000000
#define VMEM_START 0x80000000

struct VME {
  struct VME* next;

  unsigned start;
  unsigned end;

  unsigned flags;

  struct Node* file;
  unsigned file_offset;
};

// Initialize virtual memory structures
// Called once by the first core to set up global structures
void vmem_global_init(void);

// Per-core virtual memory initialization
void vmem_core_init(void);

// allocate a new page directory, with all entries invalid
unsigned create_page_directory(void);

// allocate a new page table, with all entries invalid
unsigned create_page_table(void);

// Map a file into memory, returning a pointer to the mapped region
void* mmap(unsigned size, struct Node* file, unsigned file_offset, unsigned flags);

// Unmap a previously mapped memory region
void munmap(void* p);

// free all VMEs in the given list
void free_vme_list(struct VME* vme);

// free all physical pages mapped by the given address space, 
// and free the page directory and page tables
void vmem_destroy_address_space(struct TCB* tcb);

extern void tlb_kmiss_handler_(void);
extern void tlb_umiss_handler_(void);

extern void ipi_handler_(void);

extern void mark_ipi_handled(void);

extern unsigned send_ipi(unsigned data);

#endif // VMEM_H
