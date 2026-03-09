#include "sd_driver.h"
#include "atomic.h"
#include "blocking_lock.h"
#include "debug.h"
#include "print.h"

int* DMA_MEM_REG_0 =  (int*)0x7FE5810;
int* DMA_BLOCK_REG_0 = (int*)0x7FE5814;
int* DMA_LEN_REG_0 =   (int*)0x7FE5818;
int* DMA_CTRL_REG_0 =  (int*)0x7FE581C;
int* DMA_STATUS_REG_0 = (int*)0x7FE5820;
int* DMA_ERR_REG_0 = (int*)0x7FE5824;

int* DMA_MEM_REG_1 =  (int*)0x7FE5828;
int* DMA_BLOCK_REG_1 = (int*)0x7FE582C;
int* DMA_LEN_REG_1 =   (int*)0x7FE5830;
int* DMA_CTRL_REG_1 =  (int*)0x7FE5834;
int* DMA_STATUS_REG_1 = (int*)0x7FE5838;
int* DMA_ERR_REG_1 = (int*)0x7FE583C;

// SD DMA register contract from docs/mem_map.md:
// - DMA_LEN is measured in 512-byte blocks.
// - CTRL bit 0 starts DMA and bit 3 starts SD init. Both are clear-on-write command bits.
// - STATUS/DONE/ERR are shared between DMA and SD init.
#define SD_DMA_BLOCKS_PER_TRANSFER 1
#define SD_BLOCK_SIZE_BYTES 512

#define SD_DMA_CTRL_START 0x1
#define SD_DMA_CTRL_DIR_RAM_TO_SD 0x2
#define SD_DMA_CTRL_SD_INIT 0x8

#define SD_DMA_STATUS_DONE 0x2
#define SD_DMA_STATUS_ERR 0x4

static struct BlockingLock sd_lock_0;
static struct BlockingLock sd_lock_1;

void sd_lock_acquire(enum SdDrive drive){
  if (drive == SD_DRIVE_0) {
    blocking_lock_acquire(&sd_lock_0);
  } else {
    blocking_lock_acquire(&sd_lock_1);
  }
}

void sd_lock_release(enum SdDrive drive){
  if (drive == SD_DRIVE_0) {
    blocking_lock_release(&sd_lock_0);
  } else {
    blocking_lock_release(&sd_lock_1);
  }
}

// Purpose: clear sticky DONE/ERR state before issuing a new SD command.
// Preconditions: SD MMIO is mapped and writable.
// Postconditions: DONE/ERR and DMA_ERR are cleared; BUSY is unaffected.
static void sd_clear_status(enum SdDrive drive){
  if (drive == SD_DRIVE_0) {
    *DMA_STATUS_REG_0 = 0;
  } else {
    *DMA_STATUS_REG_1 = 0;
  }
}

// Purpose: wait for shared SD status to report completion and surface failures.
// Returns: 0 on success, negative DMA error code on failure.
// Behavior: status is cleared before returning so callers can issue the next command.
static int sd_wait_done(enum SdDrive drive){
  int status;
  int err;

  do {
    if (drive == SD_DRIVE_0) {
      status = *DMA_STATUS_REG_0;
    } else {
      status = *DMA_STATUS_REG_1;
    }
  } while ((status & SD_DMA_STATUS_DONE) == 0);

  if (drive == SD_DRIVE_0) {
    err = *DMA_ERR_REG_0;
    sd_clear_status(SD_DRIVE_0);
  } else {
    err = *DMA_ERR_REG_1;
    sd_clear_status(SD_DRIVE_1);
  }

  if ((status & SD_DMA_STATUS_ERR) != 0 || err != 0){
    return -err;
  }
  return 0;
}

void sd_init(void){
  blocking_lock_init(&sd_lock_0);
  sd_clear_status(SD_DRIVE_0);
  *DMA_CTRL_REG_0 = SD_DMA_CTRL_SD_INIT;
  if (sd_wait_done(SD_DRIVE_0) != 0){
    panic("sd driver: SD_INIT command failed for drive 0\n");
  }

  blocking_lock_init(&sd_lock_1);
  sd_clear_status(SD_DRIVE_1);
  *DMA_CTRL_REG_1 = SD_DMA_CTRL_SD_INIT;
  if (sd_wait_done(SD_DRIVE_1) != 0){
    panic("sd driver: SD_INIT command failed for drive 1\n");
  }
}

int sd_read_block(enum SdDrive drive, int block_num, void* dest){
  // Set up DMA to read from SD card to memory.
  sd_clear_status(drive);
  if (drive == SD_DRIVE_0) {
    *(DMA_MEM_REG_0) = (int)dest;
    *(DMA_BLOCK_REG_0) = block_num;
    *(DMA_LEN_REG_0) = SD_DMA_BLOCKS_PER_TRANSFER;
  } else {
    *(DMA_MEM_REG_1) = (int)dest;
    *(DMA_BLOCK_REG_1) = block_num;
    *(DMA_LEN_REG_1) = SD_DMA_BLOCKS_PER_TRANSFER;
  }
  if (drive == SD_DRIVE_0) {
    *(DMA_CTRL_REG_0) = SD_DMA_CTRL_START;
  } else {
    *(DMA_CTRL_REG_1) = SD_DMA_CTRL_START;
  }
  return sd_wait_done(drive);
}

int sd_read_blocks(enum SdDrive drive, int start_block, int num_blocks, void* dest){
  if (drive == SD_DRIVE_1 && start_block == 0) {
    // warn about access to block 0
    say("| Warning: reading block 0 of drive 1\n", NULL);
  }

  sd_lock_acquire(drive);
  for(int i = 0; i < num_blocks; i++){
    int rc = sd_read_block(drive, start_block + i, (char*)dest + (i * SD_BLOCK_SIZE_BYTES));
    if (rc != 0){
      sd_lock_release(drive);
      return rc;
    }
  }
  sd_lock_release(drive);
  return 0;
}

int sd_write_block(enum SdDrive drive, int block_num, void* src){
  // Set up DMA to write from memory to SD card.
  sd_clear_status(drive);
  if (drive == SD_DRIVE_0) {
    *(DMA_MEM_REG_0) = (int)src;
    *(DMA_BLOCK_REG_0) = block_num;
    *(DMA_LEN_REG_0) = SD_DMA_BLOCKS_PER_TRANSFER;
    *(DMA_CTRL_REG_0) = SD_DMA_CTRL_START | SD_DMA_CTRL_DIR_RAM_TO_SD;
  } else {
    *(DMA_MEM_REG_1) = (int)src;
    *(DMA_BLOCK_REG_1) = block_num;
    *(DMA_LEN_REG_1) = SD_DMA_BLOCKS_PER_TRANSFER;
    *(DMA_CTRL_REG_1) = SD_DMA_CTRL_START | SD_DMA_CTRL_DIR_RAM_TO_SD;
  }
  return sd_wait_done(drive);
}

int sd_write_blocks(enum SdDrive drive, int start_block, int num_blocks, void* src){
  if (drive == SD_DRIVE_1 && start_block == 0) {
    // warn about access to block 0
    say("| Warning: writing block 0 of drive 1\n", NULL);
  }

  sd_lock_acquire(drive);
  for(int i = 0; i < num_blocks; i++){
    int rc = sd_write_block(drive, start_block + i, (char*)src + (i * SD_BLOCK_SIZE_BYTES));
    if (rc != 0){
      sd_lock_release(drive);
      return rc;
    }
  }
  sd_lock_release(drive);
  return 0;
}
