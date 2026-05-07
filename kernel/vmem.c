#include "vmem.h"
#include "machine.h"
#include "print.h"
#include "debug.h"
#include "interrupts.h"
#include "physmem.h"
#include "TCB.h"
#include "heap.h"
#include "per_core.h"
#include "page_cache.h"
#include "string.h"
#include "ivt.h"

static unsigned vmem_range_start(unsigned flags) {
  return (flags & MMAP_USER) ? USER_VMEM_START : KERNEL_VMEM_START;
}

static unsigned vmem_range_end(unsigned flags) {
  return (flags & MMAP_USER) ? USER_VMEM_END : KERNEL_VMEM_END;
}

// VMEs store an exclusive 32-bit end address. For the user half, the logical
// exclusive end would be 0x100000000, which is not representable, so the
// highest page-aligned exclusive end we can encode today is 0xFFFFF000.
static unsigned vmem_range_topdown_limit(unsigned flags) {
  unsigned range_end = vmem_range_end(flags);
  if (range_end == UINT_MAX) {
    return UINT_MAX - (FRAME_SIZE - 1);
  }
  return range_end + 1;
}

// VMEs use an exclusive end address, so the selected range must both contain
// the requested bytes and allow `start + rounded_size` to remain representable.
static bool vmem_range_can_hold(unsigned start, unsigned rounded_size,
                                unsigned range_start, unsigned range_end) {
  if (start < range_start || start > range_end) {
    return false;
  }

  if (start > UINT_MAX - rounded_size) {
    return false;
  }

  return (start + rounded_size - 1) <= range_end;
}

void vmem_global_init(void) {
  register_handler(tlb_miss_handler_, (void*)TLB_MISS_IVT_ENTRY);

  page_cache_init(&page_cache, 4096);
}

void vmem_core_init(void) {
  tlb_flush();
  set_pid(0);
}

void tlb_invalidate_range(unsigned start, unsigned end) {
  // invalidate entries on the current core's TLB
  for (unsigned va = start; va < end; va += FRAME_SIZE) {
    tlb_invalidate((void*)va);
  }
}

// allocate a new page directory for a thread
unsigned create_page_directory(void) {
  unsigned* pd = (unsigned*)physmem_alloc();
  for (int i = 0; i < 1024; i++) {
    pd[i] = 0; // mark all entries invalid
  }
  return (unsigned)pd;
}

unsigned create_page_table(void) {
  unsigned* pt = (unsigned*)physmem_alloc();
  for (int i = 0; i < 1024; i++) {
    pt[i] = 0; // mark all entries invalid
  }
  return (unsigned)pt;
}

unsigned create_zeroed_page(void) { // TODO: does this need to lock?
  unsigned* page = (unsigned*)physmem_alloc();
  for (int i = 0; i < FRAME_SIZE / sizeof(unsigned); i++) {
    page[i] = 0;
  }
  return (unsigned)page;
}

void* pte_phys_addr(unsigned pte) {
  return (void*)(pte & ~(FRAME_SIZE - 1));
}

struct VME* vme_create(unsigned start, unsigned end, unsigned size,
                       struct Node* file, unsigned file_offset, unsigned flags, unsigned paddr) {
  struct VME* vme = (struct VME*)malloc(sizeof(struct VME));

  assert(start % FRAME_SIZE == 0, "vme create: start address must be page aligned.\n");
  assert(end % FRAME_SIZE == 0, "vme create: end address must be page aligned.\n");
  assert(end > start, "vme create: end address must be greater than start address.\n");
  assert(file == NULL || (file_offset % FRAME_SIZE) == 0,
         "vme create: file-backed mmap offset must be page aligned.\n");

  if (paddr != 0) {
    assert(file == NULL, "vme create: cannot specify both a physical address and a backing file.\n");
  }

  vme->start = start;
  vme->end = end;
  vme->flags = flags;
  vme->file = node_clone(file);
  vme->file_offset = file_offset;
  vme->size = size;
  vme->paddr = paddr;

  return vme;
}

// Insert one VME into a thread's sorted, non-overlapping VME list
void vme_insert(struct TCB* tcb, struct VME* prev, struct VME* vme) {
  if (prev) {
    vme->next = prev->next;
    prev->next = vme;
  } else {
    vme->next = tcb->vme_list;
    tcb->vme_list = vme;
  }
}

// free all VMEs in the given list
void free_vme_list(struct VME* vme) {
  while (vme) {
    struct VME* next = vme->next;
    if (vme->file != NULL) {
      node_free(vme->file);
    }
    free(vme);
    vme = next;
  }
}

// unmap all physical pages backing this VME and invalidate PTE entries
void unmap_vme(unsigned* pd, struct VME* vme) {
  // free any physical pages backing this VME and invalidate PTE entries
  unsigned prev_page_dir_index = UINT_MAX;
  unsigned* prev_pt = NULL;
  for (unsigned va = vme->start; va < vme->end; va += FRAME_SIZE) {
    unsigned page_dir_index = (va >> 22) & 0x3FF;
    unsigned page_table_index = (va >> 12) & 0x3FF;

    unsigned pde = pd[page_dir_index];
    if (!(pde & VMEM_VALID))
      continue;

    unsigned* pt = (unsigned*)(pde & ~(FRAME_SIZE - 1));
    unsigned pte = pt[page_table_index];
    if (!(pte & VMEM_VALID))
      continue;

    if (vme->flags & MMAP_SHARED) {
      assert(vme->file != NULL, "cannot yet handle shared anonymous pages\n");
      // shared mapping, remove the ref
      struct Page* page = get_page((void*)pte_phys_addr(pte), "get page - unmap VME");
      physmem_page_lock(page);
      if (pt[page_table_index] == pte) { // Revalidate - ensure page didn't get evicted between finding the page and locking it
        physmem_page_removeRef(page, va, (unsigned)pd);
      }
      physmem_page_unlock(page);
    } else if (vme->paddr != 0) {
      // Direct physmem mappings borrow an existing MMIO/physical window. The
      // unmap path must only drop the translation, not return that backing page
      // to the physmem allocator.
    } else {
      // private mapping, just free the physical page
      physmem_free((void*)pte_phys_addr(pte));
    }

    pt[page_table_index] = 0;

    // only check if the page table is empty when we move to a new page directory entry
    if (page_dir_index != prev_page_dir_index && prev_pt != NULL) {
      // if page table is now empty, free it and invalidate the PDE
      bool empty = true;
      for (int i = 0; i < 1024; i++) {
        if (prev_pt[i] & VMEM_VALID) {
          empty = false;
          break;
        }
      }
      if (empty) {
        physmem_free(prev_pt);
        pd[prev_page_dir_index] = 0;
      }
    }

    prev_page_dir_index = page_dir_index;
    prev_pt = pt;
  }

  // if page table is now empty, free it and invalidate the PDE
  if (prev_pt != NULL) {
    bool empty = true;
    for (int i = 0; i < 1024; i++) {
      if (prev_pt[i] & VMEM_VALID) {
        empty = false;
        break;
      }
    }
    if (empty) {
      physmem_free(prev_pt);
      pd[prev_page_dir_index] = 0;
    }
  }

  // invalidate any TLB entries mapping this VME
  tlb_invalidate_range(vme->start, vme->end);
}

// Shared file-backed VMEs treat the rounded tail of the final page as part of
// the mapped page cache entry so writes can extend the file through mmap().
// This helper mirrors the shared-file fault path when it decides how many bytes
// of file data one cached page represents.
static unsigned shared_vme_page_bytes(struct VME* vme, unsigned va) {
  assert(vme != NULL, "shared_vme_page_bytes: VME must not be NULL.\n");
  assert(vme->file != NULL,
         "shared_vme_page_bytes: shared VME must be file-backed.\n");
  assert(va >= vme->start && va < vme->end,
         "shared_vme_page_bytes: virtual address must fall inside the VME.\n");

  unsigned vme_offset = va - vme->start;
  if (vme->size > vme_offset) {
    unsigned bytes_remaining = vme->size - vme_offset;
    if (bytes_remaining < FRAME_SIZE) {
      return bytes_remaining;
    }
  }

  return FRAME_SIZE;
}

// copy a thread's page dir/page tables and vme_list from src to dst
void vmem_fork(struct TCB* src, struct TCB* dst) {
  dst->vme_list = NULL;

  // copy vme list to dst tcb
  struct VME* prev_vme = NULL;
  for (struct VME* vme = src->vme_list; vme != NULL; vme = vme->next) {
    struct VME* new_vme = vme_create(vme->start, vme->end, vme->size,
                                     vme->file, vme->file_offset,
                                     vme->flags, vme->paddr);
    vme_insert(dst, prev_vme, new_vme);
    prev_vme = new_vme;
  }

  // copy page directory and page tables from src to dst
  dst->pid = create_page_directory();
  unsigned* src_pd = (unsigned*)src->pid;
  unsigned* dst_pd = (unsigned*)dst->pid;

  for (struct VME* vme = dst->vme_list; vme != NULL; vme = vme->next) {
    if (!(vme->flags & MMAP_USER))
      continue;

    for (unsigned va = vme->start; va < vme->end; va += FRAME_SIZE) {
      unsigned page_dir_index = (va >> 22) & 0x3FF;
      unsigned page_table_index = (va >> 12) & 0x3FF;

      unsigned pde = src_pd[page_dir_index];
      if (!(pde & VMEM_VALID))
        continue;

      unsigned* src_pt = (unsigned*)(pde & ~(FRAME_SIZE - 1));
      unsigned pte = src_pt[page_table_index];
      if (!(pte & VMEM_VALID))
        continue;

      unsigned* dst_pt;
      if (dst_pd[page_dir_index] & VMEM_VALID) {
        dst_pt = (unsigned*)(dst_pd[page_dir_index] & ~(FRAME_SIZE - 1));
      } else {
        dst_pt = (unsigned*)create_page_table();
        dst_pd[page_dir_index] = (unsigned)dst_pt | (pde & 0xFFF);
      }

      unsigned paddr = pte & ~(FRAME_SIZE - 1);
      if (vme->flags & MMAP_SHARED) {
        unsigned page_offset = vme->file_offset + (va - vme->start);
        unsigned page_bytes = shared_vme_page_bytes(vme, va);
        struct PageCacheEntry* page = page_cache_acquire(&page_cache,
                                                         vme->file, page_offset, page_bytes);
        assert((unsigned)page->page_data == paddr,
               "vmem_fork: shared source PTE must point at the page cache page.\n");
        dst_pt[page_table_index] = pte;
      } else {
        unsigned* dst_page = physmem_alloc();
        memcpy(dst_page, (void*)paddr, FRAME_SIZE);
        dst_pt[page_table_index] = (unsigned)dst_page | (pte & 0xFFF);
      }
    }
  }
}

// free all physical pages mapped by the given address space,
// and free the page directory and page tables
void vmem_destroy_address_space(struct TCB* tcb) {
  unsigned* pd = (unsigned*)tcb->pid;

  for (struct VME* vme = tcb->vme_list; vme != NULL; vme = vme->next) {
    unmap_vme(pd, vme);
  }

  // free any page tables and invalidate PDE entries
  for (unsigned page_dir_index = 0; page_dir_index < 1024; page_dir_index++) {
    if (pd[page_dir_index] & VMEM_VALID) {
      physmem_free((void*)pte_phys_addr(pd[page_dir_index]));
      pd[page_dir_index] = 0;
    }
  }

  // free the page directory itself
  physmem_free(pd);
}

// Make a VME with the given parameters and add it to the current thread's list of VMEs
void* mmap(unsigned size, struct Node* file, unsigned file_offset, unsigned flags) {
  if (size == 0)
    return NULL;

  // round up size to the nearest page boundary
  unsigned rounded_size = (size + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
  unsigned range_start = vmem_range_start(flags);
  unsigned range_end = vmem_range_end(flags);

  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  // Skip any mappings that end before the selected kernel/user half,
  // then do first-fit within that half
  struct VME* prev = NULL;
  struct VME* curr = tcb->vme_list;
  while (curr && curr->end <= range_start) {
    prev = curr;
    curr = curr->next;
  }

  unsigned last_end = range_start;
  while (curr) {
    assert(curr->start >= last_end,
           "mmap: VME list must stay sorted, non-overlapping, and stay within one address-space half.\n");

    if (curr->start > range_end) {
      break;
    }

    if (curr->start - last_end >= rounded_size) {
      break;
    }

    last_end = curr->end;
    prev = curr;
    curr = curr->next;
  }

  if (!vmem_range_can_hold(last_end, rounded_size, range_start, range_end)) {
    if (flags & MMAP_USER) {
      panic("mmap: requested mapping would exceed user virtual memory range!\n");
    } else {
      panic("mmap: requested mapping would exceed kernel virtual memory range!\n");
    }
    return NULL;
  }

  unsigned start = last_end;
  unsigned end = start + rounded_size;

  struct VME* vme = vme_create(start, end, size, file, file_offset, flags, 0);

  vme_insert(tcb, prev, vme);

  return (void*)start;
}

// Reserve an anonymous user stack using a top-down first-fit search inside the
// user half. The returned pointer is the stack base; callers still compute the
// initial SP from the top word in the reserved range.
void* mmap_stack(unsigned size, unsigned flags) {
  if (size == 0)
    return NULL;

  if (!(flags & MMAP_USER)) {
    panic("mmap_stack: user stacks must be allocated in the user virtual memory range!\n");
    return NULL;
  }

  if (flags & MMAP_SHARED) {
    panic("mmap_stack: shared anonymous stacks are not supported.\n");
    return NULL;
  }

  unsigned rounded_size = (size + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
  unsigned range_start = vmem_range_start(flags);
  unsigned range_end = vmem_range_end(flags);
  unsigned range_limit = vmem_range_topdown_limit(flags);

  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  // Walk the ascending VME list once and remember the highest gap that can fit
  // the stack. The list stays globally sorted; we only consider the user half.
  struct VME* prev = NULL;
  struct VME* curr = tcb->vme_list;
  while (curr && curr->end <= range_start) {
    prev = curr;
    curr = curr->next;
  }

  unsigned gap_start = range_start;
  unsigned stack_start = 0;
  struct VME* stack_prev = NULL;
  bool found = false;

  while (curr) {
    assert(curr->start >= gap_start,
           "mmap_stack: VME list must stay sorted, non-overlapping, and stay within one address-space half.\n");

    unsigned gap_end = curr->start;
    if (gap_end > range_limit) {
      gap_end = range_limit;
    }

    if (gap_end >= gap_start && (gap_end - gap_start) >= rounded_size) {
      stack_start = gap_end - rounded_size;
      stack_prev = prev;
      found = true;
    }

    if (curr->start >= range_limit) {
      break;
    }

    gap_start = curr->end;
    prev = curr;
    curr = curr->next;
  }

  if (gap_start <= range_limit && (range_limit - gap_start) >= rounded_size) {
    stack_start = range_limit - rounded_size;
    stack_prev = prev;
    found = true;
  }

  if (!found || !vmem_range_can_hold(stack_start, rounded_size, range_start, range_end)) {
    panic("mmap_stack: requested stack would exceed the representable user virtual memory range!\n");
    return NULL;
  }

  unsigned stack_end = stack_start + rounded_size;
  struct VME* vme = vme_create(stack_start, stack_end, size, NULL, 0, flags, 0);
  vme_insert(tcb, stack_prev, vme);

  return (void*)stack_start;
}

// Make a VME with the given parameters and add it to the current thread's list of VMEs
struct VME* mmap_at(unsigned size, struct Node* file, unsigned file_offset, unsigned flags, unsigned vaddr) {
  if (size == 0)
    return NULL;

  // round up size to the nearest page boundary
  unsigned rounded_size = (size + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
  unsigned range_start = vmem_range_start(flags);
  unsigned range_end = vmem_range_end(flags);

  if (!vmem_range_can_hold(vaddr, rounded_size, range_start, range_end)) {
    if (flags & MMAP_USER) {
      panic("mmap_at: requested mapping falls outside the user virtual memory range!\n");
    } else {
      panic("mmap_at: requested mapping falls outside the kernel virtual memory range!\n");
    }
    return NULL;
  }

  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  unsigned start = vaddr;
  unsigned end = start + rounded_size;

  // Find the first VME whose range extends past the requested start address.
  // If that VME begins before `end`, the fixed-address mapping overlaps it.
  struct VME* prev = NULL;
  struct VME* curr = tcb->vme_list;
  while (curr && curr->end <= start) {
    prev = curr;
    curr = curr->next;
  }

  if (curr != NULL && curr->start < end) {
    panic("mmap_at: requested mapping overlaps an existing VME.\n");
    return NULL;
  }

  struct VME* vme = vme_create(start, end, size, file, file_offset, flags, 0);

  vme_insert(tcb, prev, vme);

  return vme;
}

// Make a VME with the given parameters and add it to the current thread's list of VMEs
void* mmap_physmem(unsigned size, unsigned paddr, unsigned flags) {
  if (size == 0)
    return NULL;

  // round up size to the nearest page boundary
  unsigned rounded_size = (size + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
  unsigned range_start = vmem_range_start(flags);
  unsigned range_end = vmem_range_end(flags);

  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  // Skip any mappings that end before the selected kernel/user half,
  // then do first-fit within that half
  struct VME* prev = NULL;
  struct VME* curr = tcb->vme_list;
  while (curr && curr->end <= range_start) {
    prev = curr;
    curr = curr->next;
  }

  unsigned last_end = range_start;
  while (curr) {
    assert(curr->start >= last_end,
           "mmap: VME list must stay sorted, non-overlapping, and stay within one address-space half.\n");

    if (curr->start > range_end) {
      break;
    }

    if (curr->start - last_end >= rounded_size) {
      break;
    }

    last_end = curr->end;
    prev = curr;
    curr = curr->next;
  }

  if (!vmem_range_can_hold(last_end, rounded_size, range_start, range_end)) {
    if (flags & MMAP_USER) {
      panic("mmap: requested mapping would exceed user virtual memory range!\n");
    } else {
      panic("mmap: requested mapping would exceed kernel virtual memory range!\n");
    }
    return NULL;
  }

  unsigned start = last_end;
  unsigned end = start + rounded_size;

  struct VME* vme = vme_create(start, end, size, NULL, 0, flags, paddr);

  vme_insert(tcb, prev, vme);

  return (void*)start;
}

void munmap(void* p) {
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  struct VME* prev = NULL;
  struct VME* curr = tcb->vme_list;

  while (curr) {
    // find VME corresponding to p
    if ((void*)curr->start == p) {
      if (prev) {
        prev->next = curr->next;
      } else {
        tcb->vme_list = curr->next;
      }

      assert(!((curr->flags & MMAP_SHARED) && (curr->file == NULL)),
             "munmap: cannot yet unmap shared anonymous VME\n");

      // free any physical pages backing this VME
      unmap_vme((unsigned*)tcb->pid, curr);

      if (curr->file != NULL) {
        node_free(curr->file);
      }
      free(curr);
      return;
    }
    prev = curr;
    curr = curr->next;
  }

  panic("munmap called with invalid address\n");
}

unsigned* vmem_get_pte(unsigned* pd, unsigned virtual_address, bool create) {
  unsigned page_dir_index = (virtual_address >> 22) & 0x3FF;
  unsigned page_table_index = (virtual_address >> 12) & 0x3FF;
  unsigned pde = pd[page_dir_index];

  if (!(pde & VMEM_VALID)) {
    if (!create) {
      panic("vmem_get_pte: missing page table for virtual address.\n");
      return NULL;
    }

    unsigned pt_addr = create_page_table();
    pd[page_dir_index] = pt_addr | VMEM_VALID | VMEM_READ | VMEM_WRITE;
    pde = pd[page_dir_index];
  }

  unsigned* pt = (unsigned*)(pde & ~(FRAME_SIZE - 1));
  return &pt[page_table_index];
}

void vme_change_perms(struct VME* vme, unsigned new_flags) {
  vme->flags = new_flags;

  // traverse the page tables corresponding to this VME and update the permissions
  for (unsigned addr = vme->start; addr < vme->end; addr += FRAME_SIZE) {
    unsigned page_dir_index = (addr >> 22) & 0x3FF;
    unsigned page_table_index = (addr >> 12) & 0x3FF;

    unsigned* pd = get_pid();
    unsigned pde = pd[page_dir_index];
    if (!(pde & VMEM_VALID))
      continue;

    unsigned* pt = (unsigned*)(pde & ~0xFFF);
    unsigned pte = pt[page_table_index];
    if (!(pte & VMEM_VALID))
      continue;

    pte &= ~(VMEM_READ | VMEM_WRITE | VMEM_EXEC);
    if (vme->flags & MMAP_READ)
      pte |= VMEM_READ;
    if (vme->flags & MMAP_WRITE)
      pte |= VMEM_WRITE;
    if (vme->flags & MMAP_EXEC)
      pte |= VMEM_EXEC;
    if (vme->flags & MMAP_USER)
      pte |= VMEM_USER;
    pt[page_table_index] = pte;
  }

  tlb_invalidate_range(vme->start, vme->end);
}

// Walks the page table & checks the PTE.
// If the PTE is sufficient to handle the miss, updates the cache
// If the PTE is not sufficient to handle the miss (no PTE, PTE doesn't have enough permissions), calls the page fault handler
int tlb_miss_handler(void* vpn, unsigned flags, unsigned* epc_ptr, bool* return_to_user) {
  unsigned fault_addr = (unsigned)(vpn) << 12;

  unsigned* pd = get_pid();
  unsigned* pte = vmem_get_pte(pd, fault_addr, true);
  unsigned was = interrupts_disable();
  unsigned pte_value = *pte; // Lock this value in so we know it won't change throughout handling

  // Does the PTE adequately handle the miss?
  bool needs_fault = false;
  if (flags == 0) { // True TLB miss
    if (!(pte_value & VMEM_VALID))
      needs_fault = true;
  } else {
    if (flags & VMEM_READ) {
      if (pte_value & VMEM_READ) {
        flags &= ~VMEM_READ;
      } else {
        needs_fault = true;
      }
    }
    if (flags & VMEM_WRITE) {
      if (pte_value & VMEM_WRITE) {
        flags &= ~VMEM_WRITE;
      } else {
        needs_fault = true;
      }
    }
    if (flags & VMEM_EXEC) {
      if (pte_value & VMEM_EXEC) {
        flags &= ~VMEM_EXEC;
      } else {
        needs_fault = true;
      }
    }
  }

  if (!needs_fault) {
    tlb_write(fault_addr, pte_value);
    interrupts_restore(was);
    return 0;
  }
  interrupts_restore(was);

  return page_fault_handler(fault_addr, flags, pte, epc_ptr, return_to_user);
}

// Updates PTE & TLB
int page_fault_handler(unsigned fault_addr, unsigned flags, unsigned* pte, unsigned* epc_ptr, bool* return_to_user) {
  int args[2] = {fault_addr, flags};

  // look up the VME corresponding to this faulting address
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  *return_to_user = true; // default to resuming the faulting context via rfe

  struct VME* curr = tcb->vme_list;
  while (curr) {
    if (fault_addr >= curr->start && fault_addr < curr->end) {
      break;
    }
    curr = curr->next;
  }

  if (curr == NULL) {
    int args[3] = {fault_addr, flags, (int)*epc_ptr};
    say("| vmem: tlb miss fault_addr=0x%X flags=0x%X epc=0x%X has no corresponding VME\n", args);
    return segfault_helper(tcb, epc_ptr, return_to_user,
                           "| User program killed due to access of unmapped address\n",
                           "vmem: TLB miss with no corresponding VME.\n");
  }

  if ((curr->flags & MMAP_SHARED) && (curr->file == NULL)) {
    int args[3] = {fault_addr, flags, (int)*epc_ptr};
    say("| vmem: tlb miss fault_addr=0x%X flags=0x%X epc=0x%X hit unsupported shared anonymous VME\n", args);
    panic("vmem: shared anonymous TLB miss not supported yet.\n");
    return -1;
  }

  unsigned pte_value = *pte; // Make sure value is constant throughout

  if ((flags != 0) && (pte_value & VMEM_VALID)) { // Permission fault
    if (flags & VMEM_READ) {
      return segfault_helper(tcb, epc_ptr, return_to_user,
                             "| User program killed due to access of mapped page without sufficient permissions\n",
                             "vmem: can't handle a read permission fault");
    }
    if (flags & VMEM_WRITE) {
      if (!(curr->flags & MMAP_WRITE)) {
        return segfault_helper(tcb, epc_ptr, return_to_user,
                               "| User program killed due to access of mapped page without sufficient permissions\n",
                               "vmem: invalid privileges (write)");
      }
      // PTE says read-only but VME says writable => first write
      // Dirty bit tracking
      assert(pte_value != 0, "PTE value should not be 0");
      struct Page* page = get_page(pte_phys_addr(pte_value), "get page - fault handler write");
      physmem_page_lock(page);
      if (*pte == pte_value) { // Revalidate
        physmem_set_page_flags(page, PG_DIRTY);
        *pte |= VMEM_WRITE;
        tlb_write(fault_addr, pte_value); // Must update tlb_write and pte value with no possibility for eviction between
        physmem_page_unlock(page);
        return;
      }
      physmem_page_unlock(page);
      // If revalidation didn't work, fall through to invalid PTE handler
      // TODO: potentially COW (in the future)
    }
    if (flags & VMEM_EXEC) {
      return segfault_helper(tcb, epc_ptr, return_to_user,
                             "| User program killed due to access of mapped page without sufficient permissions\n",
                             "vmem: can't handle an exec permission fault");
    }
  }

  // Missing PTE
  assert(!(*pte & VMEM_VALID), "flags = 0 for existing PTE");

  // Create flags for PTE entry
  unsigned pte_flags = VMEM_VALID;
  if (curr->flags & MMAP_READ)
    pte_flags |= VMEM_READ;
  if (curr->flags & MMAP_WRITE)
    pte_flags |= VMEM_WRITE;
  if (curr->flags & MMAP_EXEC)
    pte_flags |= VMEM_EXEC;
  if (curr->flags & MMAP_USER)
    pte_flags |= VMEM_USER;

  bool allow_write = curr->flags & MMAP_WRITE;

  // Need to allocate a physical page, update the PTE, and update the TLB
  unsigned phys_page = 0;
  if (curr->file) {
    if (curr->flags & MMAP_SHARED) {
      // FILE-BACKED SHARED MAPPING
      unsigned bytes_in_page = shared_vme_page_bytes(curr, fault_addr);

      // shared mapping points directly into page cache
      struct PageCacheEntry* cache_entry = page_cache_acquire(&page_cache, curr->file, (curr->file_offset + (fault_addr - curr->start)), bytes_in_page); // Locks the page
      struct Page* page = get_page(cache_entry->page_data, "get page - file backed shared");
      physmem_page_addRef(page, fault_addr);

      pte_flags &= ~VMEM_WRITE; // Map as read-only so we can do dirty tracking on first write
      pte_value = (unsigned)cache_entry->page_data | pte_flags;
      *pte = pte_value;
      tlb_write(fault_addr, pte_value);
      physmem_page_unlock(page);
      return;
    } else {
      // FILE-BACKED PRIVATE MAPPING
      unsigned file_page_offset = curr->file_offset + (fault_addr - curr->start);
      unsigned current_size = node_size_in_bytes(curr->file);
      unsigned bytes_remaining = 0;
      if (current_size > file_page_offset) {
        bytes_remaining = current_size - file_page_offset;
      }

      unsigned bytes_in_vme = (curr->size - (fault_addr - curr->start)) > FRAME_SIZE ? FRAME_SIZE : (curr->size - (fault_addr - curr->start));
      unsigned bytes_in_page = bytes_remaining < bytes_in_vme ? bytes_remaining : bytes_in_vme;

      // private mapping copies from page cache (TODO: COW)
      struct PageCacheEntry* cache_entry = page_cache_acquire(&page_cache, curr->file,
                                                              (curr->file_offset + (fault_addr - curr->start)), bytes_in_page); // Locks the page

      phys_page = (unsigned)physmem_alloc(); // TODO: make it unpinned once we're done copying if we can evict things to swap
      memcpy((void*)phys_page, cache_entry->page_data, FRAME_SIZE);

      // Release the old page once we're done copying
      struct Page* source_page_metadata = get_page(cache_entry->page_data, "get page - fault handler file backed private");
      physmem_page_unlock(source_page_metadata);

      // TODO locking or something once swap eviction exists
      pte_value = phys_page | pte_flags;
      *pte = pte_value;
      tlb_write(fault_addr, pte_value);
      return;
    }
  } else {
    assert(!(curr->flags & MMAP_SHARED), "cannot yet handle shared anonymous pages\n");

    if (curr->paddr != 0) {
      // Physmem mappings reserve one contiguous physical window. Each faulting
      // virtual page must therefore advance through that window page-for-page
      // instead of aliasing every VME page back onto the first physical page.
      phys_page = curr->paddr + (fault_addr - curr->start);
    } else {
      // ANONYMOUS PRIVATE MAPPING
      phys_page = create_zeroed_page(); // TODO: unpin it once swap exists
    }
    // TODO locking once swap exists
    pte_value = phys_page | pte_flags;
    *pte = pte_value;
    tlb_write(fault_addr, pte_value);
    return;
  }
}

// Handles killing the user / returning to the kernel / panicking on genuinely invalid memory accesses
static int segfault_helper(struct TCB* tcb, unsigned* epc_ptr, bool* return_to_user, char* user_error_msg, char* kernel_error_msg) {

  // ISA `cr0` is the trap/exception nesting depth after entry. A value of 1
  // means this miss interrupted user mode; values above 1 mean the core was
  // already in kernel mode and took a nested miss while handling that context.
  bool was_user = get_cr0() == 1;

  if (was_user) {
    // User code touched a mapped page without sufficient permissions. Abort
    // back to the kernel caller of `jump_to_user(...)`.
    say("| User program killed due to access of mapped page without sufficient permissions\n", NULL);
    *return_to_user = false;
    return -1;
  } else if (tcb->uaccess_active) {
    // Kernel uaccess helpers recover by redirecting the faulting instruction
    // stream to their local error path, then resuming kernel mode via rfe.
    assert(tcb->uaccess_err_addr != NULL, "uaccess err addr not set");
    *epc_ptr = (unsigned)tcb->uaccess_err_addr;
    return 0;
  } else {
    panic("vmem: kernel TLB miss due to invalid privileges\n");
  }
}

void ipi_handler(unsigned data) {
  mark_ipi_handled();
  struct ShootdownRequest* request = (struct ShootdownRequest*)generic_spin_queue_remove_all(&per_core_data[get_core_id()].shootdown_requests);
  while (request != NULL) {
    struct ShootdownRequest* next = request->next;
    tlb_invalidate_other(request->pid, request->vaddr); // Handle
    countdownlatch_down(request->latch);                // Mark as handled (means element might get freed)
    request = next;
  }
}

// Block until all cores successfully shootdown this page ref
void tlb_shootdown(struct PageRef* ref) {
  unsigned num_cores = CONFIG.num_cores;
  struct ShootdownRequest** created_requests = malloc(sizeof(struct ShootdownRequest*) * num_cores); // So we can clean up the requests once everyone is done
  struct CountDownLatch latch;
  countdownlatch_init(&latch, num_cores);
  for (int i = 0; i < num_cores; i++) {
    // Create a request & give to a core
    struct ShootdownRequest* request = malloc(sizeof(struct ShootdownRequest));
    request->latch = &latch;
    request->pid = ref->pid;
    request->vaddr = (void*)ref->virtual_address;
    request->next = NULL;
    generic_spin_queue_add(&per_core_data[i].shootdown_requests, (struct GenericQueueElement*)request);

    // Keep track of created requests
    created_requests[i] = request;
  }
  // All cores IPI
  send_ipi(0); // This should interrupt us as well
  // Wait for all cores to finish shootdown
  countdownlatch_sync(&latch);
  // Clean up!
  for (int i = 0; i < num_cores; i++) {
    free(created_requests[i]);
  }
  free(created_requests);
}

// Block until all cores shootdown the linked list of page refs
// void tlb_shootdown_batch(struct PageRef* ref) {
// }

// Page must be locked already
// Page must not be pinned
void page_evict(struct Page* page) {
  assert(!(page->flags & PG_PINNED), "TRYING TO EVICT A PINNED PAGE\n");
  // Acquire the inode lock so no one can try to demand page it in before we finish writing back
  struct PageCacheEntry* cache_entry = page->cache_entry; // TODO what if someone else tries to evict at the same time as us?
  void* frame = cache_entry->page_data;
  blocking_lock_acquire(&cache_entry->key.inode->lock);

  // Evict from page cache
  page_cache_remove(&page_cache, cache_entry);

  // Remove PTEs & invlpg
  struct PageRef* ref = page->refs;
  while (ref != NULL) {
    // Overwrite the PTE
    *vmem_get_pte((unsigned*)ref->pid, ref->virtual_address, false) = 0; // TODO make sure no one can modify this pte before we do
    // Shootdown on all cores
    tlb_shootdown(ref);

    struct PageRef* to_delete = ref;
    ref = ref->next;
    free(to_delete);
    page->ref_cnt--;
  }
  assert(page->ref_cnt == 0, "page refcount != 0");
  page->refs = NULL;

  // Writeback
  if (page->flags & PG_DIRTY) {
    struct Node node;
    node.cached = cache_entry->key.inode;
    node.filesystem = &fs;
    node.parent_inumber = EXT2_BAD_INO;
    node_write_all_locked(&node, cache_entry->key.offset, FRAME_SIZE, cache_entry->page_data); // TODO does this make sense
    physmem_clear_page_flags(page, PG_DIRTY);
  }
  blocking_lock_release(&cache_entry->key.inode->lock);

  // Clean up metadata
  cache_entry->key.inode->refcount--;
  free(cache_entry);

  // Free page
  page->cache_entry = NULL;
  physmem_set_page_flags(page, PG_PINNED);
  sem_up(&page->lock);
  physmem_free(frame);
}