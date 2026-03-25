#include "print.h"
#include "sd_driver.h"

// SD card 0/1 DMA MMIO registers
int* DMA_MEM_REG_0 = (int*)0x7FE5810;
int* DMA_BLOCK_REG_0 = (int*)0x7FE5814;
int* DMA_LEN_REG_0 = (int*)0x7FE5818;
int* DMA_CTRL_REG_0 = (int*)0x7FE581C;
int* DMA_STATUS_REG_0 = (int*)0x7FE5820;
int* DMA_ERR_REG_0 = (int*)0x7FE5824;

int* DMA_MEM_REG_1 = (int*)0x7FE5828;
int* DMA_BLOCK_REG_1 = (int*)0x7FE582C;
int* DMA_LEN_REG_1 = (int*)0x7FE5830;
int* DMA_CTRL_REG_1 = (int*)0x7FE5834;
int* DMA_STATUS_REG_1 = (int*)0x7FE5838;
int* DMA_ERR_REG_1 = (int*)0x7FE583C;

// SD DMA register contract from docs/mem_map.md:
// - DMA_LEN is measured in 512-byte blocks.
// - CTRL bit 0 starts DMA and bit 3 starts SD init. Both are clear-on-write
//   command bits.
// - STATUS/DONE/ERR are shared between DMA and SD init on each drive.
#define SD_BLOCK_SIZE_BYTES 512

#define SD_DMA_CTRL_START 0x1
#define SD_DMA_CTRL_DIR_RAM_TO_SD 0x2
#define SD_DMA_CTRL_SD_INIT 0x8

#define SD_DMA_STATUS_DONE 0x2
#define SD_DMA_STATUS_ERR 0x4

// BIOS-side polling timeout for SD DMA completion.
// This is intentionally large because FPGA hardware SD init can take many
// command retries and the CPU may be slowed by clock division in some builds.
#define SD_DMA_WAIT_TIMEOUT_POLLS 50000000U
// Local software-only error code returned when the DMA never reports DONE.
#define SD_DMA_ERR_POLL_TIMEOUT 1000

// Program one SD DMA argument register
static void sd_write_arg_reg(int* reg, int value){
  *reg = value;
}

// Clear sticky DONE/ERR state before issuing a new SD command
static void sd_clear_status(enum SdDrive drive){
  if (drive == SD_DRIVE_0) {
    *DMA_STATUS_REG_0 = 0;
  } else {
    *DMA_STATUS_REG_1 = 0;
  }
}

// Emit a minimal SD DMA diagnostic when BIOS polling times out
// This avoids a silent hang on VGA/UART when one SD path deadlocks
static void sd_print_wait_timeout_diag(enum SdDrive drive, int status, int err){
  puts("| SD DMA wait timeout on drive ");
  print_num((int)drive);
  puts(". status=");
  print_num(status);
  puts(" err=");
  print_num(err);
  puts("\n");
}

// wait for one SD DMA engine to report completion and surface failures
// Returns 0 on success, negative DMA error code on failure.
static int sd_wait_done(enum SdDrive drive){
  int status;
  int err;
  unsigned polls = 0;

  do {
    if (drive == SD_DRIVE_0) {
      status = *DMA_STATUS_REG_0;
    } else {
      status = *DMA_STATUS_REG_1;
    }

    polls++;
    if (polls >= SD_DMA_WAIT_TIMEOUT_POLLS){
      if (drive == SD_DRIVE_0) {
        err = *DMA_ERR_REG_0;
      } else {
        err = *DMA_ERR_REG_1;
      }

      sd_print_wait_timeout_diag(drive, status, err);
      if (err != 0){
        return -err;
      }
      return -SD_DMA_ERR_POLL_TIMEOUT;
    }
  } while ((status & SD_DMA_STATUS_DONE) == 0);

  if (drive == SD_DRIVE_0) {
    err = *DMA_ERR_REG_0;
  } else {
    err = *DMA_ERR_REG_1;
  }
  sd_clear_status(drive);

  if ((status & SD_DMA_STATUS_ERR) != 0 || err != 0){
    return -err;
  }
  return 0;
}

// Issue SD_INIT to one drive before BIOS block IO touches it
// Returns 0 on success or a negative SD/DMA error code on failure
static int sd_init_drive(enum SdDrive drive){
  sd_clear_status(drive);
  if (drive == SD_DRIVE_0) {
    *DMA_CTRL_REG_0 = SD_DMA_CTRL_SD_INIT;
  } else {
    *DMA_CTRL_REG_1 = SD_DMA_CTRL_SD_INIT;
  }
  return sd_wait_done(drive);
}

// Initialize both SD drives before any BIOS block IO.
int sd_init(void){
  int rc = sd_init_drive(SD_DRIVE_0);
  if (rc != 0){
    return rc;
  }

  return sd_init_drive(SD_DRIVE_1);
}

// Read multiple blocks starting from the given block number into the
// destination buffer.
// The buffer must be at least num_blocks * 512 bytes.
// Returns 0 on success or a negative error code on failure.
int sd_read_blocks(enum SdDrive drive, int start_block, int num_blocks, void* dest){
  // BIOS segment tables can describe empty sections. Treat those as no-ops at
  // the software API layer even though the raw DMA engine rejects LEN == 0.
  if (num_blocks == 0) {
    return 0;
  }

  if (drive == SD_DRIVE_1 && start_block == 0) {
    puts("| Warning: reading block 0 of drive 1\n");
  }

  sd_clear_status(drive);
  if (drive == SD_DRIVE_0) {
    sd_write_arg_reg(DMA_MEM_REG_0, (int)dest);
    sd_write_arg_reg(DMA_BLOCK_REG_0, start_block);
    sd_write_arg_reg(DMA_LEN_REG_0, num_blocks);
    *DMA_CTRL_REG_0 = SD_DMA_CTRL_START;
  } else {
    sd_write_arg_reg(DMA_MEM_REG_1, (int)dest);
    sd_write_arg_reg(DMA_BLOCK_REG_1, start_block);
    sd_write_arg_reg(DMA_LEN_REG_1, num_blocks);
    *DMA_CTRL_REG_1 = SD_DMA_CTRL_START;
  }

  return sd_wait_done(drive);
}

// Write multiple blocks starting from the given block number from the source
// buffer.
// The buffer must be at least num_blocks * 512 bytes.
// Returns 0 on success or a negative error code on failure.
int sd_write_blocks(enum SdDrive drive, int start_block, int num_blocks, void* src){
  // Match sd_read_blocks(): zero-length writes are no-ops for callers even
  // though the raw DMA engine reports LEN == 0 as an error.
  if (num_blocks == 0) {
    return 0;
  }

  if (drive == SD_DRIVE_1 && start_block == 0) {
    puts("| Warning: writing block 0 of drive 1\n");
  }

  sd_clear_status(drive);
  if (drive == SD_DRIVE_0) {
    sd_write_arg_reg(DMA_MEM_REG_0, (int)src);
    sd_write_arg_reg(DMA_BLOCK_REG_0, start_block);
    sd_write_arg_reg(DMA_LEN_REG_0, num_blocks);
    *DMA_CTRL_REG_0 = SD_DMA_CTRL_START | SD_DMA_CTRL_DIR_RAM_TO_SD;
  } else {
    sd_write_arg_reg(DMA_MEM_REG_1, (int)src);
    sd_write_arg_reg(DMA_BLOCK_REG_1, start_block);
    sd_write_arg_reg(DMA_LEN_REG_1, num_blocks);
    *DMA_CTRL_REG_1 = SD_DMA_CTRL_START | SD_DMA_CTRL_DIR_RAM_TO_SD;
  }

  return sd_wait_done(drive);
}
