#ifndef SD_DRIVER_H
#define SD_DRIVER_H

#include "constants.h"
#include "TCB.h"

extern bool sd_wait_thread_0_pending; // is there about to be a thread waiting for SD drive 0?
extern struct TCB* sd_wait_thread_0; // thread waiting for SD drive 0

extern bool sd_wait_thread_1_pending; // is there about to be a thread waiting for SD drive 1?
extern struct TCB* sd_wait_thread_1; // thread waiting for SD drive 1

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

// Mark the current SD interrupt as handled, preventing duplicate interrupts
extern void mark_sd0_handled(void);
extern void mark_sd1_handled(void);

// SD interrupt handlers, defined in sd_driver.s
extern void sd0_handler_(void);
extern void sd1_handler_(void);

#endif // SD_DRIVER_H
