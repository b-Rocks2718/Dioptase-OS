#include "vmem.h"
#include "machine.h"

void vmem_global_init(void){

}

void vmem_core_init(void){
  flush_tlb();

  set_pid(0);
}
