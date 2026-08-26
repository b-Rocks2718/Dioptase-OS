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
#include "threads.h"

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

// Convert the caller's byte count into the exclusive, page-aligned extent
// stored in a VME without allowing the round-up addition to wrap. Public
// syscall validation rejects the same condition, but mmap() is also a kernel
// API and must not manufacture a zero/small VME from an oversized request.
static bool vmem_round_mapping_size(unsigned size, unsigned* rounded_size){
  if (size == 0 || size > UINT_MAX - (FRAME_SIZE - 1)){
    return false;
  }

  *rounded_size = (size + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
  return *rounded_size != 0;
}

void vmem_global_init(void){
  register_handler(tlb_miss_handler_, (void*)TLB_MISS_IVT_ENTRY);

  page_cache_init(&page_cache);
}

// to be called only by kernel_shutdown
void vmem_global_destroy(void){
  page_cache_destroy(&page_cache);
}

bool vmem_truncate_file(struct Node* node, unsigned target_size){
  // Keep the concrete global cache private to the VM implementation. The page
  // cache performs the serialized page-cache -> inode transaction and adjusts
  // all live entries for this cached inode before releasing its global lock.
  return page_cache_shrink_file(&page_cache, node, target_size);
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
  if (pd == NULL){
    return 0;
  }
  for (int i = 0; i < 1024; i++){
    pd[i] = 0; // mark all entries invalid
  }
  return (unsigned)pd;
}

unsigned create_page_table(void){
  unsigned* pt = (unsigned*)physmem_alloc();
  if (pt == NULL){
    return 0;
  }
  for (int i = 0; i < 1024; i++){
    pt[i] = 0; // mark all entries invalid
  }
  return (unsigned)pt;
}

unsigned create_zeroed_page(void){
  unsigned* page = (unsigned*)physmem_alloc();
  if (page == NULL){
    return 0;
  }
  for (int i = 0; i < FRAME_SIZE / sizeof(unsigned); i++){
    page[i] = 0;
  }
  return (unsigned)page;
}

struct VME* vme_create(unsigned start, unsigned end, unsigned size,
    struct Node* file, unsigned file_offset, unsigned flags,
    bool maps_physmem, unsigned paddr){
  struct VME* vme = (struct VME*)malloc(sizeof(struct VME));

  assert(start % FRAME_SIZE == 0, "vme create: start address must be page aligned.\n");
  assert(end % FRAME_SIZE == 0, "vme create: end address must be page aligned.\n");
  assert(end > start, "vme create: end address must be greater than start address.\n");
  assert(file == NULL || (file_offset % FRAME_SIZE) == 0,
    "vme create: file-backed mmap offset must be page aligned.\n");

  if (maps_physmem){
    assert(file == NULL,
      "vme_create: direct physical VME cannot also have a file backing.\n");
    assert(!(flags & MMAP_SHARED),
      "vme_create: direct physical VME cannot use page-cache shared ownership.\n");
  } else {
    assert(paddr == 0,
      "vme_create: non-direct VME must not retain a physical base address.\n");
  }
  
  vme->start = start;
  vme->end = end;
  vme->flags = flags;
  vme->file = node_clone(file);
  vme->file_offset = file_offset;
  vme->size = size;
  vme->maps_physmem = maps_physmem;
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
    
    if (vme->maps_physmem){
      // Direct mappings borrow an existing physical/MMIO window. This remains
      // true for inherited PTEs copied by vmem_fork(): dropping either address
      // space removes only its translation and never frees the device page.
    } else if (vme->flags & MMAP_SHARED){
      assert(vme->file != NULL, "cannot yet handle shared anonymous pages\n");
      // shared mapping, release from page cache
      page_cache_release(&page_cache, vme->file, (vme->file_offset + (va - vme->start)));
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

// Shared file-backed VMEs treat the rounded tail of the final page as part of
// the mapped page cache entry so writes can extend the file through mmap().
// This helper mirrors the shared-file fault path when it decides how many bytes
// of file data one cached page represents.
static unsigned shared_vme_page_bytes(struct VME* vme, unsigned va){
  assert(vme != NULL, "shared_vme_page_bytes: VME must not be NULL.\n");
  assert(vme->file != NULL,
    "shared_vme_page_bytes: shared VME must be file-backed.\n");
  assert(va >= vme->start && va < vme->end,
    "shared_vme_page_bytes: virtual address must fall inside the VME.\n");

  unsigned vme_offset = va - vme->start;
  if (vme->size > vme_offset){
    unsigned bytes_remaining = vme->size - vme_offset;
    if (bytes_remaining < FRAME_SIZE){
      return bytes_remaining;
    }
  }

  return FRAME_SIZE;
}

static void vmem_destroy_address_space_impl(struct TCB* tcb);

// copy a thread's page dir/page tables and vme_list from src to dst
bool vmem_fork(struct TCB* src, struct TCB* dst){
  dst->vme_list = NULL;
  dst->pid = 0;

  // copy vme list to dst tcb
  struct VME* prev_vme = NULL;
  for (struct VME* vme = src->vme_list; vme != NULL; vme = vme->next){
    struct VME* new_vme = vme_create(vme->start, vme->end, vme->size,
                                     vme->file, vme->file_offset,
                                     vme->flags, vme->maps_physmem,
                                     vme->paddr);
    vme_insert(dst, prev_vme, new_vme);
    prev_vme = new_vme;
  }

  // copy page directory and page tables from src to dst
  dst->pid = create_page_directory();
  if (dst->pid == 0){
    free_vme_list(dst->vme_list);
    dst->vme_list = NULL;
    return false;
  }
  unsigned* src_pd = (unsigned*)src->pid;
  unsigned* dst_pd = (unsigned*)dst->pid;

  for (struct VME* vme = dst->vme_list; vme != NULL; vme = vme->next){
    // Ordinary kernel VMEs retain the historical metadata-only inheritance.
    // Direct VMEs are different: the live-alias contract applies regardless
    // of whether their virtual address is exposed to user mode, so copy any
    // resident direct PTE instead of silently changing its residency semantics.
    if (!(vme->flags & MMAP_USER) && !vme->maps_physmem) continue;

    for (unsigned va = vme->start; va < vme->end; va += FRAME_SIZE){
      unsigned page_dir_index = (va >> 22) & 0x3FF;
      unsigned page_table_index = (va >> 12) & 0x3FF;

      unsigned pde = src_pd[page_dir_index];
      if (!(pde & VMEM_VALID)) continue;

      unsigned* src_pt = (unsigned*)(pde & ~(FRAME_SIZE - 1));
      unsigned pte = src_pt[page_table_index];
      if (!(pte & VMEM_VALID)) continue;

      unsigned* dst_pt;
      if (dst_pd[page_dir_index] & VMEM_VALID){
        dst_pt = (unsigned*)(dst_pd[page_dir_index] & ~(FRAME_SIZE - 1));
      } else {
        dst_pt = (unsigned*)create_page_table();
        if (dst_pt == NULL){
          vmem_destroy_address_space_impl(dst);
          free_vme_list(dst->vme_list);
          dst->vme_list = NULL;
          dst->pid = 0;
          return false;
        }
        dst_pd[page_dir_index] = (unsigned)dst_pt | (pde & 0xFFF);
      }

      unsigned paddr = pte & ~(FRAME_SIZE - 1);
      if (vme->maps_physmem){
        // A direct VME is a borrowed live device/physical alias, not a private
        // RAM snapshot. Copy the valid source translation verbatim. Nonresident
        // pages remain absent and will independently fault to the same physical
        // window through the cloned VME metadata.
        unsigned expected_paddr = vme->paddr + (va - vme->start);
        if (paddr != expected_paddr){
          int args[3] = {(int)va, (int)paddr, (int)expected_paddr};
          say("| vmem: fork direct-map mismatch va=0x%X pte_paddr=0x%X expected_paddr=0x%X\n",
            args);
          panic("vmem_fork: resident direct-map PTE does not match its VME physical window.\n");
        }
        dst_pt[page_table_index] = pte;
      } else if (vme->flags & MMAP_SHARED){
        struct PageCacheEntry* page = page_cache_acquire(&page_cache,
          vme->file, vme->file_offset + (va - vme->start));
        if (page == NULL){
          vmem_destroy_address_space_impl(dst);
          free_vme_list(dst->vme_list);
          dst->vme_list = NULL;
          dst->pid = 0;
          return false;
        }
        assert((unsigned)page->page_data == paddr,
          "vmem_fork: shared source PTE must point at the page cache page.\n");
        dst_pt[page_table_index] = pte;
      } else {
        unsigned* dst_page = physmem_alloc();
        if (dst_page == NULL){
          vmem_destroy_address_space_impl(dst);
          free_vme_list(dst->vme_list);
          dst->vme_list = NULL;
          dst->pid = 0;
          return false;
        }
        memcpy(dst_page, (void*)paddr, FRAME_SIZE);
        dst_pt[page_table_index] = (unsigned)dst_page | (pte & 0xFFF);
      }
    }
  }

  return true;
}

// free all physical pages mapped by the given address space, 
// and free the page directory and page tables
static void vmem_destroy_address_space_impl(struct TCB* tcb) {
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

void vmem_destroy_address_space(struct TCB* tcb) {
  vmem_destroy_address_space_impl(tcb);
}

// Make a VME with the given parameters and add it to the current thread's list of VMEs
void* mmap(unsigned size, struct Node* file, unsigned file_offset, unsigned flags){
  unsigned rounded_size = 0;
  if (!vmem_round_mapping_size(size, &rounded_size)){
    return NULL;
  }
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
    // A sorted list with no sufficiently large gap is normal finite-resource
    // exhaustion. The syscall layer translates NULL to -1; kernel callers
    // must likewise unwind their operation rather than dereference address 0.
    return NULL;
  }

  unsigned start = last_end;
  unsigned end = start + rounded_size;

  struct VME* vme = vme_create(start, end, size, file, file_offset, flags,
    false, 0);

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
    // Validated ELF layouts reserve the stack interval, but ordinary exhaustion
    // after a partial load must remain a fallible construction error.
    return NULL;
  }

  unsigned stack_end = stack_start + rounded_size;
  struct VME* vme = vme_create(stack_start, stack_end, size, NULL, 0, flags,
    false, 0);
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
    // Fixed-address callers (ELF load) treat this as a recoverable construction
    // failure so exec can roll back to the pre-exec address space.
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
    return NULL;
  }

  struct VME* vme = vme_create(start, end, size, file, file_offset, flags,
    false, 0);

  vme_insert(tcb, prev, vme);

  return vme;
}

// Map one page-aligned physical window into the current TCB. This is a
// kernel-only operation: malformed arguments indicate a kernel caller bug,
// while failure to find a virtual gap is ordinary finite-resource exhaustion.
void* mmap_physmem(unsigned size, unsigned paddr, unsigned flags){
  unsigned rounded_size = 0;
  if (!vmem_round_mapping_size(size, &rounded_size)){
    int args[2] = {(int)paddr, (int)size};
    say("| vmem: mmap_physmem invalid request paddr=0x%X size=%u: size cannot be page-rounded representably\n",
      args);
    panic("vmem: mmap_physmem kernel caller supplied an invalid mapping size.\n");
    return NULL;
  }

  if ((paddr & (FRAME_SIZE - 1)) != 0){
    int args[2] = {(int)paddr, FRAME_SIZE};
    say("| vmem: mmap_physmem invalid paddr=0x%X: expected %u-byte alignment\n",
      args);
    panic("vmem: mmap_physmem kernel caller supplied an unaligned physical address.\n");
    return NULL;
  }

  if (paddr >= PHYS_ADDR_END_EXCLUSIVE ||
      rounded_size > PHYS_ADDR_END_EXCLUSIVE - paddr){
    int args[3] = {(int)paddr, (int)rounded_size,
      PHYS_ADDR_END_EXCLUSIVE};
    say("| vmem: mmap_physmem physical range paddr=0x%X rounded_size=%u exceeds exclusive_end=0x%X\n",
      args);
    panic("vmem: mmap_physmem kernel caller supplied an out-of-range physical window.\n");
    return NULL;
  }

  unsigned allowed_flags = MMAP_READ | MMAP_WRITE | MMAP_EXEC | MMAP_USER;
  if (flags & ~allowed_flags){
    int args[2] = {(int)flags, (int)paddr};
    say("| vmem: mmap_physmem invalid flags=0x%X for paddr=0x%X\n",
      args);
    panic("vmem: mmap_physmem kernel caller supplied shared or unknown flags.\n");
    return NULL;
  }

  unsigned range_start = vmem_range_start(flags);
  unsigned range_end = vmem_range_end(flags);

  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  // Exact direct-map requests are idempotent per TCB. This is required for
  // getter-style APIs with no user-visible munmap operation: repeated calls
  // must neither change their pointer nor consume virtual address space.
  for (struct VME* existing = tcb->vme_list; existing != NULL;
      existing = existing->next){
    if (existing->maps_physmem && existing->paddr == paddr &&
        existing->size == size && existing->flags == flags &&
        existing->end - existing->start == rounded_size){
      return (void*)existing->start;
    }
  }

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
      "mmap_physmem: VME list must stay sorted, non-overlapping, and stay within one address-space half.\n");

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
    // Running out of virtual range is a normal resource failure. Trap callers
    // translate NULL to -1; kernel callers must unwind without dereferencing 0.
    return NULL;
  }

  unsigned start = last_end;
  unsigned end = start + rounded_size;

  struct VME* vme = vme_create(start, end, size, NULL, 0, flags, true, paddr);

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
      // The TLB exception wrapper retains the faulting EPC/EFG on the kernel
      // stack while the handler runs. sigreturn() therefore retries the same
      // user access; handlers that cannot repair the mapping must exit.
      if (try_run_current_signal_handler(SIGNAL_SEG, (unsigned)vpn, flags)){
        return 0;
      }

      // User code touched a mapped page without sufficient permissions. Abort
      // back to the kernel caller of `jump_to_user(...)`.
      say("| vmem: user access killed: mapped page lacks required permissions\n",
        NULL);
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
      if (try_run_current_signal_handler(SIGNAL_SEG, (unsigned)vpn, flags)){
        return 0;
      }

      // User code touched an unmapped address. Abort back to the kernel caller
      // of `jump_to_user(...)`.
      int args[2] = {fault_addr, (int)*epc_ptr};
      say("| vmem: user access killed: unmapped address=0x%X epc=0x%X\n",
        args);
      *return_to_user = false;
      return -1;
    } else if (tcb->uaccess_active){
      // Kernel uaccess helpers recover by redirecting the faulting instruction
      // stream to their local error path, then resuming kernel mode via rfe.
      assert(tcb->uaccess_err_addr != NULL, "uaccess err addr not set");
      *epc_ptr = (unsigned)tcb->uaccess_err_addr;
      return 0;
    } else {
      int args[3] = {fault_addr, flags, (int)*epc_ptr};
      say("| vmem: tlb miss fault_addr=0x%X flags=0x%X epc=0x%X has no corresponding VME\n", args);
      panic("vmem: TLB miss with no corresponding VME.\n");
      return -1;
    }
  }

  if ((curr->flags & MMAP_SHARED) && (curr->file == NULL)){
    int args[3] = {fault_addr, flags, (int)*epc_ptr};
    say("| vmem: tlb miss fault_addr=0x%X flags=0x%X epc=0x%X hit unsupported shared anonymous VME\n", args);
    panic("vmem: shared anonymous TLB miss not supported yet.\n");
    return -1;
  }

  unsigned page_dir_index = ((unsigned)vpn >> 10) & 0x3FF;
  
  unsigned* pd = get_pid();
  unsigned pde = pd[page_dir_index];

  if (!(pde & VMEM_VALID)) {
    // need to create a new page table for this PDE
    unsigned pt_addr = create_page_table();
    if (pt_addr == 0){
      goto physmem_exhausted;
    }
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
    if (curr->maps_physmem){
      // Direct VMEs are borrowed physical/MMIO aliases. They never acquire,
      // dirty, or release page-cache entries, regardless of their permissions.
      phys_page = curr->paddr + (fault_addr - curr->start);
    } else if (curr->file){
      if (curr->flags & MMAP_SHARED){
        // shared mapping points directly into page cache
        struct PageCacheEntry* page = page_cache_acquire(&page_cache, curr->file, 
          curr->file_offset + (fault_addr - curr->start));
        if (page == NULL){
          goto physmem_exhausted;
        }
        if (curr->flags & MMAP_WRITE){
          // Conservatively treating writable exposure as dirty is intentional:
          // the ISA provides no later dirty transition to observe, and this is
          // how shared mmap is allowed to extend a file. Read-only aliases do
          // not publish or enlarge this writeback extent.
          unsigned bytes_in_page = shared_vme_page_bytes(curr, fault_addr);
          page_cache_mark_dirty(&page_cache, curr->file,
            curr->file_offset + (fault_addr - curr->start), bytes_in_page);
        }
        phys_page = (unsigned)page->page_data;
      } else {
        unsigned file_page_offset = curr->file_offset + (fault_addr - curr->start);

        // private mapping copies from page cache (TODO: COW)
        struct PageCacheEntry* page = page_cache_acquire(&page_cache, curr->file, 
          file_page_offset);
        if (page == NULL){
          goto physmem_exhausted;
        }
        
        phys_page = (unsigned)physmem_alloc();
        if (phys_page == 0){
          page_cache_release(&page_cache, curr->file, file_page_offset);
          goto physmem_exhausted;
        }
        memcpy((void*)phys_page, page->page_data, FRAME_SIZE);

        page_cache_release(&page_cache, curr->file, 
          (curr->file_offset + (fault_addr - curr->start)));
      }
    } else {
      assert(!(curr->flags & MMAP_SHARED), "cannot yet handle shared anonymous pages\n");
      phys_page = create_zeroed_page();
      if (phys_page == 0){
        goto physmem_exhausted;
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

physmem_exhausted:
  {
    int args[2] = {fault_addr, (int)*epc_ptr};
    say("| vmem: tlb miss out of physical pages fault_addr=0x%X epc=0x%X\n",
      args);
  }
  if (was_user){
    if (try_run_current_signal_handler(SIGNAL_SEG, (unsigned)vpn, flags)){
      return 0;
    }
    *return_to_user = false;
    return -1;
  }
  if (tcb->uaccess_active){
    assert(tcb->uaccess_err_addr != NULL, "uaccess err addr not set");
    *epc_ptr = (unsigned)tcb->uaccess_err_addr;
    return 0;
  }
  panic("vmem: kernel TLB miss exhausted physical memory.\n");
  return -1;
}

void ipi_handler(unsigned data){
  mark_ipi_handled();

  int cid = get_core_id();
  int args[2] = {cid, data};
  say("| Received IPI on core %d with data %d\n", args);
}
