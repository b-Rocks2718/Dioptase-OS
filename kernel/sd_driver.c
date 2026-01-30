#include "sd_driver.h"
#include "atomic.h"


int* DMA_MEM_REG =  (int*)0x7FE58F0;
int* DMA_BLOCK_REG = (int*)0x7FE58F4;
int* DMA_LEN_REG =   (int*)0x7FE58F8;
int* DMA_CTRL_REG =  (int*)0x7FE58FC;
int* DMA_STATUS_REG = (int*)0x7FE5900;

static int sd_lock = 0;

int sd_read_block(int block_num, void* dest){
  // Set up DMA to read from SD card to memory
  *(DMA_MEM_REG) = (int)dest;
  *(DMA_BLOCK_REG) = block_num;
  *(DMA_LEN_REG) = 512; // Assuming block size is 512 bytes
  *(DMA_CTRL_REG) = 1; // Start DMA read

  // Wait for DMA to complete
  while(*DMA_STATUS_REG != 2);

  return 0; // Success
}

int sd_read_blocks(int start_block, int num_blocks, void* dest){
  spin_lock_get(&sd_lock);
  for(int i = 0; i < num_blocks; i++){
    sd_read_block(start_block + i, (char*)dest + (i * 512));
  }
  spin_lock_release(&sd_lock);
  return 0; // Success
}

int sd_write_block(int block_num, void* src){
  // Set up DMA to write from memory to SD card
  *(DMA_MEM_REG) = (int)src;
  *(DMA_BLOCK_REG) = block_num;
  *(DMA_LEN_REG) = 512; // Assuming block size is 512 bytes
  *(DMA_CTRL_REG) = 3; // Start DMA write

  // Wait for DMA to complete
  while(*DMA_STATUS_REG != 2);

  return 0; // Success
}

int sd_write_blocks(int start_block, int num_blocks, void* src){
  spin_lock_get(&sd_lock);
  for(int i = 0; i < num_blocks; i++){
    sd_write_block(start_block + i, (char*)src + (i * 512));
  }
  spin_lock_release(&sd_lock);
  return 0; // Success
}
