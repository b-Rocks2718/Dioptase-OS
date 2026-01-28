#include "machine.h"
#include "print.h"

extern void kernel_main(void);

void kernel_entry(void){
  if (get_core_id() == 0){
    puts("| Hello from kernel!\n");

    kernel_main();
  } else {
    while (1);
  }
}