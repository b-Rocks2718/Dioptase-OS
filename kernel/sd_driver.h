#ifndef SD_DRIVER_H
#define SD_DRIVER_H

enum SdDrive {
  SD_DRIVE_0 = 0,
  SD_DRIVE_1 = 1,
};

void sd_init(void);

int sd_read_block(enum SdDrive drive, int block_num, void* dest);
int sd_read_blocks(enum SdDrive drive, int start_block, int num_blocks, void* dest);

int sd_write_block(enum SdDrive drive, int block_num, void* src);
int sd_write_blocks(enum SdDrive drive, int start_block, int num_blocks, void* src);

#endif // SD_DRIVER_H
