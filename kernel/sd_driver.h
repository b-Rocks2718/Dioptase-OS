#ifndef SD_DRIVER_H
#define SD_DRIVER_H

// Enum of the SD ports on the Dioptase board
enum SdDrive {
  SD_DRIVE_0 = 0,
  SD_DRIVE_1 = 1,
};

// Initialize the SD driver and both drives. This must be called before any other SD functions
void sd_init(void);

// Read multiple blocks starting from the given block number into the destination buffer
// The buffer must be at least num_blocks * 512 bytes
// Returns 0 on success or a negative error code on failure
int sd_read_blocks(enum SdDrive drive, int start_block, int num_blocks, void* dest);

// Write multiple blocks starting from the given block number from the source buffer
// The buffer must be at least num_blocks * 512 bytes
// Returns 0 on success or a negative error code on failure
int sd_write_blocks(enum SdDrive drive, int start_block, int num_blocks, void* src);

#endif // SD_DRIVER_H
