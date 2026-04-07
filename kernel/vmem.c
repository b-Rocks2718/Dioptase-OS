#include "vmem.h"
#include "machine.h"
#include "print.h"
#include "debug.h"
#include "interrupts.h"
#include "physmem.h"

unsigned* test_page = NULL;

void vmem_global_init(void){
  register_handler(tlb_kmiss_handler_, (void*)0x20C);
  register_handler(tlb_umiss_handler_, (void*)0x208);

  test_page = (unsigned*)physmem_alloc();
  *test_page = 42;
}

void vmem_core_init(void){
  tlb_flush();

  set_pid(0);
}

// Map a file into memory, returning a pointer to the mapped region
// If the file is NULL, do an anonymous mapping
void* mmap(unsigned size, bool shared, struct Node* file, unsigned file_offset){

}

void munmap(void* p){

}

void tlb_kmiss_handler(void* vpn){
  printf("Kernel TLB miss vpn %X!\n", &vpn);

  unsigned fault_addr = (unsigned)vpn << 12;
  
  static unsigned count = 0;
  count++;

  unsigned entry = (unsigned)test_page;
  entry |= TLB_READ;

  if (count > 1){
    entry |= TLB_WRITE;
  }
  
  tlb_write(fault_addr, entry);
}

void tlb_umiss_handler(void* vpn){
  printf("User TLB miss vpn %X!\n", &vpn);
}
