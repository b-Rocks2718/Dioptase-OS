#include "vmem.h"
#include "machine.h"

void vmem_global_init(void){

}

void vmem_core_init(void){
  flush_tlb();

  set_pid(0);
}

// Map a file into memory, returning a pointer to the mapped region
// If the file is NULL, do an anonymous mapping
void* mmap(unsigned size, bool shared, struct Node* file, unsigned file_offset){

}

void munmap(void* p){

}
