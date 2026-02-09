
int* DMA_MEM_REG =  (int*)0x7FE5810;
int* DMA_BLOCK_REG = (int*)0x7FE5814;
int* DMA_LEN_REG =   (int*)0x7FE5818;
int* DMA_CTRL_REG =  (int*)0x7FE581C;
int* DMA_STATUS_REG = (int*)0x7FE5820;

// SD DMA register contract from docs/mem_map.md:
// DMA_LEN is measured in 512-byte blocks, not bytes.
#define SD_DMA_BLOCKS_PER_TRANSFER 1
#define SD_BLOCK_SIZE_BYTES 512

int sd_read_block(int block_num, void* dest){
  // Set up DMA to read from SD card to memory
  *(DMA_MEM_REG) = (int)dest;
  *(DMA_BLOCK_REG) = block_num;
  *(DMA_LEN_REG) = SD_DMA_BLOCKS_PER_TRANSFER;
  *(DMA_CTRL_REG) = 1; // Start DMA read

  // Wait for DMA to complete
  while(*DMA_STATUS_REG != 2);

  return 0; // Success
}

int sd_read_blocks(int start_block, int num_blocks, void* dest){
  for(int i = 0; i < num_blocks; i++){
    sd_read_block(start_block + i, (char*)dest + (i * SD_BLOCK_SIZE_BYTES));
  }
  return 0; // Success
}

int sd_write_block(int block_num, void* src){
  // Set up DMA to write from memory to SD card
  *(DMA_MEM_REG) = (int)src;
  *(DMA_BLOCK_REG) = block_num;
  *(DMA_LEN_REG) = SD_DMA_BLOCKS_PER_TRANSFER;
  *(DMA_CTRL_REG) = 3; // Start DMA write

  // Wait for DMA to complete
  while(*DMA_STATUS_REG != 2);

  return 0; // Success
}

int sd_write_blocks(int start_block, int num_blocks, void* src){
  for(int i = 0; i < num_blocks; i++){
    sd_write_block(start_block + i, (char*)src + (i * SD_BLOCK_SIZE_BYTES));
  }
  return 0; // Success
}
