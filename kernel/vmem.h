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
#define MMAP_USER   0x20

// tlb/pde/pte flags
#define VMEM_READ   0x01
#define VMEM_WRITE  0x02
#define VMEM_EXEC   0x04
#define VMEM_USER   0x08
#define VMEM_GLOBAL 0x10
#define VMEM_VALID  0x20
#define VMEM_DIRTY  0x40

// begin vmem allocations from 0x10000000
#define KERNEL_VMEM_START 0x10000000
#define KERNEL_VMEM_END   0x7FFFFFFF

#define USER_VMEM_START 0x80000000
#define USER_VMEM_END   0xFFFFFFFF

// The ISA and physical memory map define a 27-bit, 128 MiB physical address
// space. This constant is the exclusive end used for checked physical-window
// arithmetic; the highest valid physical byte address is 0x07FFFFFF.
#define PHYS_ADDR_END_EXCLUSIVE 0x08000000

struct VME {
  struct VME* next;

  unsigned start;
  unsigned end;

  unsigned size;

  unsigned flags;

  struct Node* file;
  unsigned file_offset;

  // Direct physical VMEs borrow this physical/MMIO window; they never own or
  // free its pages. Keep an explicit kind bit because physical address zero is
  // architecturally valid and cannot serve as a "not direct" sentinel.
  bool maps_physmem;
  unsigned paddr;
};

// Initialize virtual memory structures
// Called once by the first core to set up global structures
void vmem_global_init(void);

// Destroy virtual-memory global synchronization after all address spaces stop
void vmem_global_destroy(void);

/*
 * Shrink a regular file while serializing the inode EOF update with the global
 * VM page cache. This may block and is not interrupt-context safe. Returns
 * false for a growth request; on success, already-dirty cached pages cannot
 * later write back beyond target_size unless a subsequent writable mapping
 * fault deliberately publishes a new extent.
 */
bool vmem_truncate_file(struct Node* node, unsigned target_size);

// Per-core virtual memory initialization
void vmem_core_init(void);

// allocate a new page directory, with all entries invalid
unsigned create_page_directory(void);

// allocate a new page table, with all entries invalid
unsigned create_page_table(void);

// Map a file into memory, returning a pointer to the mapped region. Returns
// NULL when size cannot be page-rounded representably or no range can fit.
void* mmap(unsigned size, struct Node* file, unsigned file_offset, unsigned flags);

// Reserve an anonymous user stack mapping from the top of the user address
// space downward, returning the stack's lowest virtual address.
void* mmap_stack(unsigned size, unsigned flags);

// Map a file into memory at a specific virtual address, returning a pointer to the vme
struct VME* mmap_at(unsigned size, struct Node* file, unsigned file_offset, unsigned flags, unsigned vaddr);

/*
 * Map a physical memory region into the current thread's virtual address space.
 *
 * Preconditions (kernel-only API):
 * - size is nonzero and page-roundable without unsigned overflow
 * - paddr is FRAME_SIZE-aligned and the rounded window lies wholly below
 *   PHYS_ADDR_END_EXCLUSIVE
 * - flags contains only READ/WRITE/EXEC/USER; direct mappings are inherently
 *   live aliases and must not use the page-cache MMAP_SHARED ownership flag
 * - execution is in ordinary kernel/trap context, not interrupt context; the
 *   current TCB exclusively owns mutation of its VME list
 *
 * An invalid kernel request panics with its size/address/flags. An ordinary
 * lack of virtual address space returns NULL. Repeating an exact request in the
 * same TCB returns the existing VME base instead of reserving another range.
 * Direct mappings are borrowed, inherited across fork as live aliases, and
 * never returned to physmem by munmap or address-space teardown.
 */
void* mmap_physmem(unsigned size, unsigned paddr, unsigned flags);

// Unmap a previously mapped memory region
void munmap(void* p);

// free all VMEs in the given list
void free_vme_list(struct VME* vme);

/*
 * Clone src's page directory, page tables, and VME list into dst.
 *
 * Preconditions: execution is in ordinary kernel mode; src's address space is
 * stable and is not concurrently mutated; dst is not runnable and owns no VME
 * list/page directory that needs preservation. fork_tcb() establishes these
 * conditions before publishing the child to the scheduler.
 *
 * Postconditions on success: dst owns independent VME metadata/page tables.
 * Private resident pages are copied, shared file pages retain page-cache
 * references, and resident direct physical/MMIO PTEs are copied as borrowed
 * live aliases. On failure returns false after freeing any partial dst state;
 * the caller must not publish the child.
 */
bool vmem_fork(struct TCB* src, struct TCB* dst);

// free all physical pages mapped by the given address space, 
// and free the page directory and page tables
void vmem_destroy_address_space(struct TCB* tcb);

void vme_change_perms(struct VME* vme, unsigned new_flags);

extern void tlb_miss_handler_(void);

extern void ipi_handler_(void);

extern void mark_ipi_handled(void);

extern unsigned send_ipi(unsigned data);

#endif // VMEM_H
