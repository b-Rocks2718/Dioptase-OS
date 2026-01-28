#include "print.h"
#include "machine.h"
#include "sd_driver.h"

unsigned char* MBR_LOAD_ADDRESS = (unsigned char*)0x800000;

int bios_entry(void){
  puts("| Hello from Dioptase BIOS!\n");

  puts("| Looking for boot device...\n");
  sd_read_block(0, MBR_LOAD_ADDRESS);

  if (MBR_LOAD_ADDRESS[510] != 0x55 || MBR_LOAD_ADDRESS[511] != 0xAA){
    puts("| No valid MBR found on boot device. Halting.\n");
    return 1;
  }

  puts("| Valid MBR found. Loading kernel...\n");
  unsigned kernel_start_block = ((unsigned*)MBR_LOAD_ADDRESS)[0];
  unsigned kernel_num_blocks = ((unsigned*)MBR_LOAD_ADDRESS)[1];
  unsigned kernel_load_address = ((unsigned*)MBR_LOAD_ADDRESS)[2];

  sd_read_blocks(kernel_start_block, kernel_num_blocks, (unsigned char*)kernel_load_address);

  puts("| Entering kernel...\n");
  enter_kernel((void*)kernel_load_address);

  return 0;
}