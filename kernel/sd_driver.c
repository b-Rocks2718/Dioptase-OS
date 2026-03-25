#include "sd_driver.h"
#include "atomic.h"
#include "blocking_lock.h"
#include "debug.h"
#include "print.h"
#include "threads.h"
#include "interrupts.h"
#include "per_core.h"
#include "machine.h"

// SD MMIO addresses 

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
#define SD_BLOCK_SIZE_BYTES 512

#define SD_DMA_CTRL_START 0x1
#define SD_DMA_CTRL_DIR_RAM_TO_SD 0x2
#define SD_DMA_CTRL_SD_INIT 0x8

#define SD_DMA_STATUS_DONE 0x2
#define SD_DMA_STATUS_ERR 0x4

static struct BlockingLock sd_lock_0;
static struct BlockingLock sd_lock_1;

// get the BlockingLock for the given drive
void sd_lock_acquire(enum SdDrive drive){
  if (drive == SD_DRIVE_0) {
    blocking_lock_acquire(&sd_lock_0);
  } else {
    blocking_lock_acquire(&sd_lock_1);
  }
}

// release the BlockingLock for the given drive
void sd_lock_release(enum SdDrive drive){
  if (drive == SD_DRIVE_0) {
    blocking_lock_release(&sd_lock_0);
  } else {
    blocking_lock_release(&sd_lock_1);
  }
}

// clear sticky DONE/ERR state before issuing a new SD command.
static void sd_clear_status(enum SdDrive drive){
  if (drive == SD_DRIVE_0) {
    *DMA_STATUS_REG_0 = 0;
  } else {
    *DMA_STATUS_REG_1 = 0;
  }
}

// thread function for blocking on SD commands. Will be passed to block() in sd_wait_done()
// stores the threads waiting for each drive in per-core data 
// so that the SD interrupt handler can wake them up when the command is done
void sd_block_thread(void* arg){
  int* args = (int*)arg;
  enum SdDrive drive = (enum SdDrive)args[0];
  struct TCB* tcb = (struct TCB*)args[1];

  int was = interrupts_disable();
  if (drive == SD_DRIVE_0) {
    struct PerCore* per_core = get_per_core();
    assert(per_core->sd_wait_thread_0 == NULL, 
      "sd_block_thread: sd_wait_thread_0 is not NULL");
    per_core->sd_wait_thread_0 = tcb;
  } else {
    struct PerCore* per_core = get_per_core();
    assert(per_core->sd_wait_thread_1 == NULL, 
      "sd_block_thread: sd_wait_thread_1 is not NULL");
    per_core->sd_wait_thread_1 = tcb;
  }
  interrupts_restore(was);
}

// wait for shared SD status to report completion and surface failures.
static int sd_wait_done(enum SdDrive drive){
  int status;
  int err;

  if (__atomic_load_n(&bootstrapping) || true) { // blocking is TODO
    // During bootstrapping, we don't have threads or interrupts set up yet, 
    // so we have to busy wait
    do {
      if (drive == SD_DRIVE_0) {
        status = *DMA_STATUS_REG_0;
      } else {
        status = *DMA_STATUS_REG_1;
      }
    } while ((status & SD_DMA_STATUS_DONE) == 0);
  } else {
    // block until we get an interrupt from the SD controller indicating the command is done
    int was = interrupts_disable(); 
    struct TCB* current_tcb = get_current_tcb();
    int args[2] = { (int)drive, (int)current_tcb };
    block(was, sd_block_thread, (void*)(args));
  }

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

// Initialize the SD driver and both drives. This must be called before any other SD functions
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

// Read multiple blocks starting from the given block number into the destination buffer
// The buffer must be at least num_blocks * 512 bytes
// Returns 0 on success or a negative error code on failure
int sd_read_blocks(enum SdDrive drive, int start_block, int num_blocks, void* dest){
  if (drive == SD_DRIVE_1 && start_block == 0) {
    // warn about access to block 0
    say("| Warning: reading block 0 of drive 1\n", NULL);
  }

  sd_lock_acquire(drive);

  // Set up DMA to read from SD card to memory.
  sd_clear_status(drive);
  if (drive == SD_DRIVE_0) {
    *(DMA_MEM_REG_0) = (int)dest;
    *(DMA_BLOCK_REG_0) = start_block;
    *(DMA_LEN_REG_0) = num_blocks;
  } else {
    *(DMA_MEM_REG_1) = (int)dest;
    *(DMA_BLOCK_REG_1) = start_block;
    *(DMA_LEN_REG_1) = num_blocks;
  }
  if (drive == SD_DRIVE_0) {
    *(DMA_CTRL_REG_0) = SD_DMA_CTRL_START;
  } else {
    *(DMA_CTRL_REG_1) = SD_DMA_CTRL_START;
  }
  int rc = sd_wait_done(drive);

  sd_lock_release(drive);

  return rc;
}

// Write multiple blocks starting from the given block number from the source buffer
// The buffer must be at least num_blocks * 512 bytes
// Returns 0 on success or a negative error code on failure
int sd_write_blocks(enum SdDrive drive, int start_block, int num_blocks, void* src){
  if (drive == SD_DRIVE_1 && start_block == 0) {
    // warn about access to block 0
    say("| Warning: writing block 0 of drive 1\n", NULL);
  }

  sd_lock_acquire(drive);

  // Set up DMA to write from memory to SD card.
  sd_clear_status(drive);
  if (drive == SD_DRIVE_0) {
    *(DMA_MEM_REG_0) = (int)src;
    *(DMA_BLOCK_REG_0) = start_block;
    *(DMA_LEN_REG_0) = num_blocks;
    *(DMA_CTRL_REG_0) = SD_DMA_CTRL_START | SD_DMA_CTRL_DIR_RAM_TO_SD;
  } else {
    *(DMA_MEM_REG_1) = (int)src;
    *(DMA_BLOCK_REG_1) = start_block;
    *(DMA_LEN_REG_1) = num_blocks;
    *(DMA_CTRL_REG_1) = SD_DMA_CTRL_START | SD_DMA_CTRL_DIR_RAM_TO_SD;
  }
  int rc = sd_wait_done(drive);

  sd_lock_release(drive);

  return rc;
}
