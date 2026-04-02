#include "print.h"
#include "machine.h"
#include "sd_driver.h"
#include "config.h"
#include "constants.h"

// Temporary buffer for SD block 0 while the BIOS validates and decodes the
// current boot-sector metadata contract.
unsigned char* MBR_LOAD_ADDRESS = (unsigned char*)0x5000;

// init.s is the first code to run on the CPU. After setting up a stack, it calls bios_entry()
// Loads the kernel image described by SD block 0 and transfers control to its text entry point
int bios_entry(void){
  vga_text_init();

  puts("| Hello from Dioptase BIOS!\n");

  puts("| Looking for boot device...\n");

  int sd_rc = sd_init();
  if (sd_rc != 0){
    puts("| SD initialization failed (rc=");
    print_num(sd_rc);
    puts("). Halting.\n");
    // Keep the VGA message visible on screen. Headless boots return to init.s,
    // which halts immediately after bios_entry returns.
    if (CONFIG.use_vga) while (1);
    return 1;
  }
  
  sd_rc = sd_read_blocks(SD_DRIVE_0, 0, 1, MBR_LOAD_ADDRESS);
  if (sd_rc != 0){
    puts("| Failed to read MBR from SD card (rc=");
    print_num(sd_rc);
    puts("). Halting.\n");
    if (CONFIG.use_vga) while (1);
    return 1;
  }

  // The BIOS expects an MBR-style 0x55AA trailer in SD block 0
  if (MBR_LOAD_ADDRESS[510] != 0x55 ||
      MBR_LOAD_ADDRESS[511] != 0xAA){
    puts("| No valid MBR found on boot device. Halting.\n");
    if (CONFIG.use_vga) while (1);
    return 1;
  }

  puts("| Valid MBR found. Loading kernel...\n");
  // boot-sector word layout:
  // [0..2]  text   = start_block, num_blocks, load_address
  // [3..5]  data   = start_block, num_blocks, load_address
  // [6..8]  rodata = start_block, num_blocks, load_address
  // [9..10] bss    = num_blocks, load_address
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

  if (sd_read_blocks(SD_DRIVE_0, text_start_block, text_num_blocks, (unsigned char*)text_load_address) != 0){
    puts("| Failed to read kernel text from SD card. Halting.\n");
    return 1;
  }
  if (sd_read_blocks(SD_DRIVE_0, data_start_block, data_num_blocks, (unsigned char*)data_load_address) != 0){
    puts("| Failed to read kernel data from SD card. Halting.\n");
    return 1;
  }
  if (sd_read_blocks(SD_DRIVE_0, rodata_start_block, rodata_num_blocks, (unsigned char*)rodata_load_address) != 0){
    puts("| Failed to read kernel rodata from SD card. Halting.\n");
    return 1;
  }

  // BSS is described in whole SD blocks, but the kernel expects zeroed bytes.
  unsigned bss_size = bss_num_blocks * SD_BLOCK_SIZE_BYTES;
  unsigned char* bss_ptr = (unsigned char*)bss_load_address;
  for (unsigned i = 0; i < bss_size; ++i){
    bss_ptr[i] = 0;
  }

  puts("| Entering kernel...\n");
  enter_kernel((void*)text_load_address);

  return 0;
}
