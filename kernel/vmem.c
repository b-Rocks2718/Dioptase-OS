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

struct PageCache page_cache;

static unsigned vmem_range_start(unsigned flags){
  return (flags & MMAP_USER) ? USER_VMEM_START : KERNEL_VMEM_START;
}

static unsigned vmem_range_end(unsigned flags){
  return (flags & MMAP_USER) ? USER_VMEM_END : KERNEL_VMEM_END;
}

// VMEs store an exclusive 32-bit end address. For the user half, the logical
// exclusive end would be 0x100000000, which is not representable, so the
// highest page-aligned exclusive end we can encode today is 0xFFFFF000.
static unsigned vmem_range_topdown_limit(unsigned flags){
  unsigned range_end = vmem_range_end(flags);
  if (range_end == UINT_MAX){
    return UINT_MAX - (FRAME_SIZE - 1);
  }
  return range_end + 1;
}

// VMEs use an exclusive end address, so the selected range must both contain
// the requested bytes and allow `start + rounded_size` to remain representable.
static bool vmem_range_can_hold(unsigned start, unsigned rounded_size,
    unsigned range_start, unsigned range_end){
  if (start < range_start || start > range_end){
    return false;
  }

  if (start > UINT_MAX - rounded_size){
    return false;
  }

  return (start + rounded_size - 1) <= range_end;
}

void vmem_global_init(void){
  register_handler(tlb_miss_handler_, (void*)TLB_MISS_IVT_ENTRY);

  page_cache_init(&page_cache, 4096);
}

void vmem_core_init(void){
  tlb_flush();
  set_pid(0);
}

void tlb_invalidate_range(unsigned start, unsigned end){
  // invalidate entries on the current core's TLB
  for (unsigned va = start; va < end; va += FRAME_SIZE){
    tlb_invalidate((void*)va);
  }
}

// allocate a new page directory for a thread
unsigned create_page_directory(void){
  unsigned* pd = (unsigned*)physmem_alloc();
  for (int i = 0; i < 1024; i++){
    pd[i] = 0; // mark all entries invalid
  }
  return (unsigned)pd;
}

unsigned create_page_table(void){
  unsigned* pt = (unsigned*)physmem_alloc();
  for (int i = 0; i < 1024; i++){
    pt[i] = 0; // mark all entries invalid
  }
  return (unsigned)pt;
}

unsigned create_zeroed_page(void){
  unsigned* page = (unsigned*)physmem_alloc();
  for (int i = 0; i < FRAME_SIZE / sizeof(unsigned); i++){
    page[i] = 0;
  }
  return (unsigned)page;
}

struct VME* vme_create(unsigned start, unsigned end, unsigned size,
    struct Node* file, unsigned file_offset, unsigned flags, unsigned paddr){
  struct VME* vme = (struct VME*)malloc(sizeof(struct VME));

  assert(start % FRAME_SIZE == 0, "vme create: start address must be page aligned.\n");
  assert(end % FRAME_SIZE == 0, "vme create: end address must be page aligned.\n");
  assert(end > start, "vme create: end address must be greater than start address.\n");
  assert(file == NULL || (file_offset % FRAME_SIZE) == 0,
    "vme create: file-backed mmap offset must be page aligned.\n");

  if (paddr != 0){
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
void vme_insert(struct TCB* tcb, struct VME* prev, struct VME* vme){
  if (prev){
    vme->next = prev->next;
    prev->next = vme;
  } else {
    vme->next = tcb->vme_list;
    tcb->vme_list = vme;
  }
}

// free all VMEs in the given list
void free_vme_list(struct VME* vme){
  while (vme){
    struct VME* next = vme->next;
    if (vme->file != NULL){
      node_free(vme->file);
    }
    free(vme);
    vme = next;
  }
}

// unmap all physical pages backing this VME and invalidate PTE entries
void unmap_vme(unsigned* pd, struct VME* vme){
  // free any physical pages backing this VME and invalidate PTE entries
  unsigned prev_page_dir_index = UINT_MAX;
  unsigned* prev_pt = NULL;
  for (unsigned va = vme->start; va < vme->end; va += FRAME_SIZE) {
    unsigned page_dir_index = (va >> 22) & 0x3FF;
    unsigned page_table_index = (va >> 12) & 0x3FF;

    unsigned pde = pd[page_dir_index];
    if (!(pde & VMEM_VALID)) continue;

    unsigned* pt = (unsigned*)(pde & ~(FRAME_SIZE - 1));
    unsigned pte = pt[page_table_index];
    if (!(pte & VMEM_VALID)) continue;
    
    if (vme->flags & MMAP_SHARED){
      assert(vme->file != NULL, "cannot yet handle shared anonymous pages\n");
      // shared mapping, release from page cache
      page_cache_release(&page_cache, vme->file, (vme->file_offset + (va - vme->start)));
    } else if (vme->paddr != 0){
      // Direct physmem mappings borrow an existing MMIO/physical window. The
      // unmap path must only drop the translation, not return that backing page
      // to the physmem allocator.
    } else {
      // private mapping, just free the physical page
      physmem_free((void*)(pte & ~(FRAME_SIZE - 1)));
    }
    
    pt[page_table_index] = 0;

    // only check if the page table is empty when we move to a new page directory entry
    if (page_dir_index != prev_page_dir_index && prev_pt != NULL){
      // if page table is now empty, free it and invalidate the PDE
      bool empty = true;
      for (int i = 0; i < 1024; i++){
        if (prev_pt[i] & VMEM_VALID){
          empty = false;
          break;
        }
      }
      if (empty){
        physmem_free(prev_pt);
        pd[prev_page_dir_index] = 0;
      }
    }

    prev_page_dir_index = page_dir_index;
    prev_pt = pt;
  }

  // if page table is now empty, free it and invalidate the PDE
  if (prev_pt != NULL){
    bool empty = true;
    for (int i = 0; i < 1024; i++){
      if (prev_pt[i] & VMEM_VALID){
        empty = false;
        break;
      }
    }
    if (empty){
      physmem_free(prev_pt);
      pd[prev_page_dir_index] = 0;
    }
  }

  // invalidate any TLB entries mapping this VME
  tlb_invalidate_range(vme->start, vme->end);
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
      physmem_free((void*)(pd[page_dir_index] & ~(FRAME_SIZE - 1)));
      pd[page_dir_index] = 0;
    }
  }

  // free the page directory itself
  physmem_free(pd);
}

// Make a VME with the given parameters and add it to the current thread's list of VMEs
void* mmap(unsigned size, struct Node* file, unsigned file_offset, unsigned flags){
  if (size == 0) return NULL;

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
  while (curr && curr->end <= range_start){
    prev = curr;
    curr = curr->next;
  }

  unsigned last_end = range_start;
  while (curr){
    assert(curr->start >= last_end,
      "mmap: VME list must stay sorted, non-overlapping, and stay within one address-space half.\n");

    if (curr->start > range_end){
      break;
    }

    if (curr->start - last_end >= rounded_size){
      break;
    }

    last_end = curr->end;
    prev = curr;
    curr = curr->next;
  }

  if (!vmem_range_can_hold(last_end, rounded_size, range_start, range_end)){
    if (flags & MMAP_USER){
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
void* mmap_stack(unsigned size, unsigned flags){
  if (size == 0) return NULL;

  if (!(flags & MMAP_USER)){
    panic("mmap_stack: user stacks must be allocated in the user virtual memory range!\n");
    return NULL;
  }

  if (flags & MMAP_SHARED){
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
  while (curr && curr->end <= range_start){
    prev = curr;
    curr = curr->next;
  }

  unsigned gap_start = range_start;
  unsigned stack_start = 0;
  struct VME* stack_prev = NULL;
  bool found = false;

  while (curr){
    assert(curr->start >= gap_start,
      "mmap_stack: VME list must stay sorted, non-overlapping, and stay within one address-space half.\n");

    unsigned gap_end = curr->start;
    if (gap_end > range_limit){
      gap_end = range_limit;
    }

    if (gap_end >= gap_start && (gap_end - gap_start) >= rounded_size){
      stack_start = gap_end - rounded_size;
      stack_prev = prev;
      found = true;
    }

    if (curr->start >= range_limit){
      break;
    }

    gap_start = curr->end;
    prev = curr;
    curr = curr->next;
  }

  if (gap_start <= range_limit && (range_limit - gap_start) >= rounded_size){
    stack_start = range_limit - rounded_size;
    stack_prev = prev;
    found = true;
  }

  if (!found || !vmem_range_can_hold(stack_start, rounded_size, range_start, range_end)){
    panic("mmap_stack: requested stack would exceed the representable user virtual memory range!\n");
    return NULL;
  }

  unsigned stack_end = stack_start + rounded_size;
  struct VME* vme = vme_create(stack_start, stack_end, size, NULL, 0, flags, 0);
  vme_insert(tcb, stack_prev, vme);

  return (void*)stack_start;
}

// Make a VME with the given parameters and add it to the current thread's list of VMEs
struct VME* mmap_at(unsigned size, struct Node* file, unsigned file_offset, unsigned flags, unsigned vaddr){
  if (size == 0) return NULL;

  // round up size to the nearest page boundary
  unsigned rounded_size = (size + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
  unsigned range_start = vmem_range_start(flags);
  unsigned range_end = vmem_range_end(flags);

  if (!vmem_range_can_hold(vaddr, rounded_size, range_start, range_end)){
    if (flags & MMAP_USER){
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
  while (curr && curr->end <= start){
    prev = curr;
    curr = curr->next;
  }

  if (curr != NULL && curr->start < end){
    panic("mmap_at: requested mapping overlaps an existing VME.\n");
    return NULL;
  }

  struct VME* vme = vme_create(start, end, size, file, file_offset, flags, 0);

  vme_insert(tcb, prev, vme);

  return vme;
}

// Make a VME with the given parameters and add it to the current thread's list of VMEs
void* mmap_physmem(unsigned size, unsigned paddr, unsigned flags){
  if (size == 0) return NULL;

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
  while (curr && curr->end <= range_start){
    prev = curr;
    curr = curr->next;
  }

  unsigned last_end = range_start;
  while (curr){
    assert(curr->start >= last_end,
      "mmap: VME list must stay sorted, non-overlapping, and stay within one address-space half.\n");

    if (curr->start > range_end){
      break;
    }

    if (curr->start - last_end >= rounded_size){
      break;
    }

    last_end = curr->end;
    prev = curr;
    curr = curr->next;
  }

  if (!vmem_range_can_hold(last_end, rounded_size, range_start, range_end)){
    if (flags & MMAP_USER){
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

void munmap(void* p){
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  struct VME* prev = NULL;
  struct VME* curr = tcb->vme_list;

  while (curr){
    // find VME corresponding to p
    if ((void*)curr->start == p){
      if (prev){
        prev->next = curr->next;
      } else {
        tcb->vme_list = curr->next;
      }

      assert(!((curr->flags & MMAP_SHARED) && (curr->file == NULL)), 
        "munmap: cannot yet unmap shared anonymous VME\n");

      // free any physical pages backing this VME
      unmap_vme((unsigned*)tcb->pid, curr);

      if (curr->file != NULL){
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

void vme_change_perms(struct VME* vme, unsigned new_flags){
  vme->flags = new_flags;

  // traverse the page tables corresponding to this VME and update the permissions
  for (unsigned addr = vme->start; addr < vme->end; addr += FRAME_SIZE){
    unsigned page_dir_index = (addr >> 22) & 0x3FF;
    unsigned page_table_index = (addr >> 12) & 0x3FF;

    unsigned* pd = get_pid();
    unsigned pde = pd[page_dir_index];
    if (!(pde & VMEM_VALID)) continue;

    unsigned* pt = (unsigned*)(pde & ~0xFFF);
    unsigned pte = pt[page_table_index];
    if (!(pte & VMEM_VALID)) continue;

    pte &= ~(VMEM_READ | VMEM_WRITE | VMEM_EXEC);
    if (vme->flags & MMAP_READ) pte |= VMEM_READ;
    if (vme->flags & MMAP_WRITE) pte |= VMEM_WRITE;
    if (vme->flags & MMAP_EXEC) pte |= VMEM_EXEC;
    if (vme->flags & MMAP_USER) pte |= VMEM_USER;
    pt[page_table_index] = pte;
  }

  tlb_invalidate_range(vme->start, vme->end);
}

int tlb_miss_handler(void* vpn, unsigned flags, unsigned* epc_ptr, bool* return_to_user){
  // look up the VME corresponding to this faulting address
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  // ISA `cr0` is the trap/exception nesting depth after entry. A value of 1
  // means this miss interrupted user mode; values above 1 mean the core was
  // already in kernel mode and took a nested miss while handling that context.
  bool was_user = get_cr0() == 1;
  *return_to_user = true; // default to resuming the faulting context via rfe

  if (flags != 0){
    if (was_user){
      // User code touched a mapped page without sufficient permissions. Abort
      // back to the kernel caller of `jump_to_user(...)`.
      say("User program killed due to access of mapped page without sufficient permissions\n", NULL);
      *return_to_user = false;
      return -1;
    } else if (tcb->uaccess_active){
      // Kernel uaccess helpers recover by redirecting the faulting instruction
      // stream to their local error path, then resuming kernel mode via rfe.
      assert(tcb->uaccess_err_addr != NULL, "uaccess err addr not set");
      *epc_ptr = (unsigned)tcb->uaccess_err_addr;
      return 0;
    } else {
      panic("vmem: kernel TLB miss due to invalid privileges\n");
    }
  }

  unsigned fault_addr = (unsigned)(vpn) << 12;

  struct VME* curr = tcb->vme_list;
  while (curr){
    if (fault_addr >= curr->start && fault_addr < curr->end){
      break;
    }
    curr = curr->next;
  }

  if (curr == NULL){
    if (was_user) {
      // User code touched an unmapped address. Abort back to the kernel caller
      // of `jump_to_user(...)`.
      say("User program killed due to access of unmapped address\n", NULL);
      *return_to_user = false;
      return -1;
    } else if (tcb->uaccess_active){
      // Kernel uaccess helpers recover by redirecting the faulting instruction
      // stream to their local error path, then resuming kernel mode via rfe.
      assert(tcb->uaccess_err_addr != NULL, "uaccess err addr not set");
      *epc_ptr = (unsigned)tcb->uaccess_err_addr;
      return 0;
    } else {
      int args[2] = {fault_addr, flags};
      say("| vmem: tlb miss fault_addr=0x%X flags=0x%X has no corresponding VME\n", args);
      panic("vmem: TLB miss with no corresponding VME.\n");
      return -1;
    }
  }

  if ((curr->flags & MMAP_SHARED) && (curr->file == NULL)){
    int args[2] = {fault_addr, flags};
    say("| vmem: tlb miss fault_addr=0x%X flags=0x%X hit unsupported shared anonymous VME\n", args);
    panic("vmem: shared anonymous TLB miss not supported yet.\n");
    return -1;
  }

  unsigned page_dir_index = ((unsigned)vpn >> 10) & 0x3FF;
  
  unsigned* pd = get_pid();
  unsigned pde = pd[page_dir_index];

  if (!(pde & VMEM_VALID)) {
    // need to create a new page table for this PDE
    unsigned pt_addr = create_page_table();
    unsigned entry = pt_addr | VMEM_VALID | VMEM_READ | VMEM_WRITE;
    pd[page_dir_index] = entry;
    pde = entry;
  }

  unsigned* pt = (unsigned*)(pde & ~0xFFF);
  unsigned page_table_index = (unsigned)vpn & 0x3FF;
  unsigned pte = pt[page_table_index];

  if (!(pte & VMEM_VALID)) {
    // need to allocate a physical page and update the PTE
    unsigned phys_page = 0;
    if (curr->file){
      if (curr->flags & MMAP_SHARED){
        // this is intentional, so mmap can be used to extend files
        unsigned bytes_in_page =  (curr->size - (fault_addr - curr->start)) > FRAME_SIZE ? 
          FRAME_SIZE : (curr->size - (fault_addr - curr->start));

        // shared mapping points directly into page cache
        struct PageCacheEntry* page = page_cache_acquire(&page_cache, curr->file, 
          (curr->file_offset + (fault_addr - curr->start)), bytes_in_page);
        if (curr->flags & MMAP_WRITE){
          page_cache_mark_dirty(&page_cache, curr->file,
            (curr->file_offset + (fault_addr - curr->start)));
        }
        phys_page = (unsigned)page->page_data;
      } else {
        unsigned file_page_offset = curr->file_offset + (fault_addr - curr->start);
        unsigned current_size = node_size_in_bytes(curr->file);
        unsigned bytes_remaining = 0;
        if (current_size > file_page_offset){
          bytes_remaining = current_size - file_page_offset;
        }

        unsigned bytes_in_vme = (curr->size - (fault_addr - curr->start)) > FRAME_SIZE ?
          FRAME_SIZE : (curr->size - (fault_addr - curr->start));
        unsigned bytes_in_page = bytes_remaining < bytes_in_vme ?
          bytes_remaining : bytes_in_vme;

        // private mapping copies from page cache (TODO: COW)
        struct PageCacheEntry* page = page_cache_acquire(&page_cache, curr->file, 
          (curr->file_offset + (fault_addr - curr->start)), bytes_in_page);
        
        phys_page = (unsigned)physmem_alloc();
        memcpy((void*)phys_page, page->page_data, FRAME_SIZE);

        page_cache_release(&page_cache, curr->file, 
          (curr->file_offset + (fault_addr - curr->start)));
      }
    } else {
      assert(!(curr->flags & MMAP_SHARED), "cannot yet handle shared anonymous pages\n");
      
      if (curr->paddr != 0){
        // Physmem mappings reserve one contiguous physical window. Each faulting
        // virtual page must therefore advance through that window page-for-page
        // instead of aliasing every VME page back onto the first physical page.
        phys_page = curr->paddr + (fault_addr - curr->start);
      } else {
        phys_page = create_zeroed_page();
      }
    }
    
    unsigned entry = phys_page | VMEM_VALID;

    if (curr->flags & MMAP_READ) entry |= VMEM_READ;
    if (curr->flags & MMAP_WRITE) entry |= VMEM_WRITE;
    if (curr->flags & MMAP_EXEC) entry |= VMEM_EXEC;
    if (curr->flags & MMAP_USER) entry |= VMEM_USER;
    pt[page_table_index] = entry;
    pte = entry;
  }
  
  tlb_write(fault_addr, pte);
  return 0;
}

void ipi_handler(unsigned data){
  mark_ipi_handled();

  int cid = get_core_id();
  int args[2] = {cid, data};
  say("| Received IPI on core %d with data %d\n", args);
}
