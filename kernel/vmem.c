#include "vmem.h"
#include "machine.h"
#include "print.h"
#include "debug.h"
#include "interrupts.h"
#include "physmem.h"
#include "TCB.h"
#include "heap.h"
#include "per_core.h"

void vmem_global_init(void){
  register_handler(tlb_kmiss_handler_, (void*)0x20C);
  register_handler(tlb_umiss_handler_, (void*)0x208);
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

struct VME* vme_create(unsigned start, unsigned end,
    bool shared, struct Node* file, unsigned file_offset){
  struct VME* vme = (struct VME*)malloc(sizeof(struct VME));

  assert(start % FRAME_SIZE == 0, "vme create: start address must be page aligned.\n");
  assert(end % FRAME_SIZE == 0, "vme create: end address must be page aligned.\n");
  assert(end > start, "vme create: end address must be greater than start address.\n");
  
  vme->start = start;
  vme->end = end;
  vme->shared = shared;
  vme->file = file;
  vme->file_offset = file_offset;

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
    free(vme);
    vme = next;
  }
}

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

    physmem_free((void*)(pte & ~(FRAME_SIZE - 1)));
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
    assert(vme->file == NULL && !vme->shared,
      "vmem destroy: shared/file-backed VME reclaim is not implemented.\n");

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
void* mmap(unsigned size, bool shared, struct Node* file, unsigned file_offset){
  // round up size to the nearest page boundary
  size = (size + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);

  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  // traverse vme list to find a free virtual memory region
  struct VME* prev = NULL;
  struct VME* curr = tcb->vme_list;
  unsigned last_end = VMEM_START;
  while (curr){
    assert(curr->start >= last_end,
      "mmap: VME list must stay sorted and non-overlapping.\n");

    if (curr->start - last_end >= size){
      break;
    }
    last_end = curr->end;
    prev = curr;
    curr = curr->next;
  }

  if (curr == NULL && (UINT_MAX - last_end) < size){
    // no more virtual memory
    panic("Out of virtual memory!");
    return NULL;
  }

  unsigned start = last_end;
  unsigned end = start + size;

  struct VME* vme = vme_create(start, end, shared, file, file_offset);

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

      assert(!curr->shared, "munmap: cannot yet unmap shared VME\n");

      // free any physical pages backing this VME
      unmap_vme((unsigned*)tcb->pid, curr);

      free(curr);
      return;
    }
    prev = curr;
    curr = curr->next;
  }

  panic("munmap called with invalid address\n");
}

void tlb_kmiss_handler(void* vpn){
  unsigned fault_addr = (unsigned)(vpn) << 12;

  // look up the VME corresponding to this faulting address
  int was = interrupts_disable();
  struct TCB* tcb = get_current_tcb();
  interrupts_restore(was);

  struct VME* curr = tcb->vme_list;
  while (curr){
    if (fault_addr >= curr->start && fault_addr < curr->end){
      break;
    }
    curr = curr->next;
  }

  if (curr == NULL){
    panic("Kernel TLB miss with no corresponding VME!\n");
    return;
  }

  if (curr->shared){
    panic("Kernel TLB miss on shared VME not supported yet!\n");
    return;
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
      phys_page = (unsigned)physmem_alloc();
      node_read_all(curr->file, curr->file_offset, FRAME_SIZE, (char*)phys_page);
    } else {
      phys_page = create_zeroed_page();
    }
    
    unsigned entry = phys_page | VMEM_VALID | VMEM_READ | VMEM_WRITE;
    pt[page_table_index] = entry;
    pte = entry;
  }
  
  tlb_write(fault_addr, pte);
}

void tlb_umiss_handler(void* vpn){
  panic("User TLB miss!\n");
}

void ipi_handler(unsigned data){
  mark_ipi_handled();

  int cid = get_core_id();
  int args[2] = {cid, data};
  say("| Received IPI on core %d with data %d\n", args);
}
