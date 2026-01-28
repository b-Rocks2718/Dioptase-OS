#include "interrupts.h"
#include "print.h"

void spurious_interrupt_handler(void){
  puts("| Spurious interrupt received?\n");
}
