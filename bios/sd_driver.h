#ifndef SD_DRIVER_H
#define SD_DRIVER_H

int sd_init(void);

int sd_read_block(int block_num, void* dest);
int sd_read_blocks(int start_block, int num_blocks, void* dest);

int sd_write_block(int block_num, void* src);
int sd_write_blocks(int start_block, int num_blocks, void* src);

#endif // SD_DRIVER_H
