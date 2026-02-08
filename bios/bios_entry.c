#include "print.h"
#include "machine.h"
#include "sd_driver.h"

unsigned char* MBR_LOAD_ADDRESS = (unsigned char*)0x800000;

int bios_entry(void){
  vga_text_init();

  puts("| Hello from Dioptase BIOS!\n");

  puts("| Looking for boot device...\n");
  sd_read_block(0, MBR_LOAD_ADDRESS);

  if (MBR_LOAD_ADDRESS[510] != 0x55 || MBR_LOAD_ADDRESS[511] != 0xAA){
    puts("| No valid MBR found on boot device. Halting.\n");
    return 1;
  }

  puts("| Valid MBR found. Loading kernel...\n");
  unsigned text_start_block = ((unsigned*)MBR_LOAD_ADDRESS)[0];
  unsigned text_num_blocks = ((unsigned*)MBR_LOAD_ADDRESS)[1];
  unsigned text_load_address = ((unsigned*)MBR_LOAD_ADDRESS)[2];

  unsigned data_start_block = ((unsigned*)MBR_LOAD_ADDRESS)[3];
  unsigned data_num_blocks = ((unsigned*)MBR_LOAD_ADDRESS)[4];
  unsigned data_load_address = ((unsigned*)MBR_LOAD_ADDRESS)[5];

  unsigned rodata_start_block = ((unsigned*)MBR_LOAD_ADDRESS)[6];
  unsigned rodata_num_blocks = ((unsigned*)MBR_LOAD_ADDRESS)[7];
  unsigned rodata_load_address = ((unsigned*)MBR_LOAD_ADDRESS)[8];

  unsigned bss_num_blocks = ((unsigned*)MBR_LOAD_ADDRESS)[9];
  unsigned bss_load_address = ((unsigned*)MBR_LOAD_ADDRESS)[10];

  sd_read_blocks(text_start_block, text_num_blocks, (unsigned char*)text_load_address);
  sd_read_blocks(data_start_block, data_num_blocks, (unsigned char*)data_load_address);
  sd_read_blocks(rodata_start_block, rodata_num_blocks, (unsigned char*)rodata_load_address);

  // zero out bss
  unsigned bss_size = bss_num_blocks * 512;
  unsigned char* bss_ptr = (unsigned char*)bss_load_address;
  for (unsigned i = 0; i < bss_size; ++i){
    bss_ptr[i] = 0;
  }

  puts("| Entering kernel...\n");
  enter_kernel((void*)text_load_address);

  return 0;
}