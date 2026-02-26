#include "print.h"

int* DMA_MEM_REG = (int*)0x7FE5810;
int* DMA_BLOCK_REG = (int*)0x7FE5814;
int* DMA_LEN_REG = (int*)0x7FE5818;
int* DMA_CTRL_REG = (int*)0x7FE581C;
int* DMA_STATUS_REG = (int*)0x7FE5820;
int* DMA_ERR_REG = (int*)0x7FE5824;

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

// BIOS-side polling timeout for SD DMA completion.
// This is intentionally large because FPGA hardware SD init can take many
// command retries and the CPU may be slowed by clock division in some builds.
#define SD_DMA_WAIT_TIMEOUT_POLLS 50000000U
// Local software-only error code returned when the DMA never reports DONE.
#define SD_DMA_ERR_POLL_TIMEOUT 1000

// Purpose: program one SD DMA argument register.
// Hardware contract: one 32-bit store must fully program the target register.
static void sd_write_arg_reg(int* reg, int value){
  *reg = value;
}

// Purpose: clear sticky DONE/ERR state before issuing a new SD command.
// Preconditions: SD MMIO is mapped and writable.
// Postconditions: DONE/ERR and DMA_ERR are cleared; BUSY is unaffected.
static void sd_clear_status(void){
  *DMA_STATUS_REG = 0;
}

// Purpose: emit a minimal SD DMA diagnostic when BIOS polling times out.
// This avoids a silent hang on VGA/UART when the FPGA SD path deadlocks.
static void sd_print_wait_timeout_diag(int status, int err){
  puts("| SD DMA wait timeout. status=");
  print_num(status);
  puts(" err=");
  print_num(err);
  puts("\n");
}

// Purpose: wait for shared SD status to report completion and surface failures.
// Returns: 0 on success, negative DMA error code on failure.
// Behavior:
// - status is cleared before returning on normal completion/error.
// - on timeout, status is left intact so diagnostics remain visible in MMIO.
static int sd_wait_done(void){
  int status;
  int err;
  unsigned polls = 0;

  do {
    status = *DMA_STATUS_REG;
    polls++;
    if (polls >= SD_DMA_WAIT_TIMEOUT_POLLS){
      err = *DMA_ERR_REG;
      sd_print_wait_timeout_diag(status, err);
      if (err != 0){
        return -err;
      }
      return -SD_DMA_ERR_POLL_TIMEOUT;
    }
  } while ((status & SD_DMA_STATUS_DONE) == 0);

  err = *DMA_ERR_REG;
  sd_clear_status();

  if ((status & SD_DMA_STATUS_ERR) != 0 || err != 0){
    return -err;
  }
  return 0;
}

int sd_init(void){
  sd_clear_status();
  *DMA_CTRL_REG = SD_DMA_CTRL_SD_INIT;
  return sd_wait_done();
}

int sd_read_block(int block_num, void* dest){
  sd_clear_status();
  sd_write_arg_reg(DMA_MEM_REG, (int)dest);
  sd_write_arg_reg(DMA_BLOCK_REG, block_num);
  sd_write_arg_reg(DMA_LEN_REG, SD_DMA_BLOCKS_PER_TRANSFER);
  *DMA_CTRL_REG = SD_DMA_CTRL_START;
  return sd_wait_done();
}

int sd_read_blocks(int start_block, int num_blocks, void* dest){
  for (int i = 0; i < num_blocks; i++){
    int rc = sd_read_block(start_block + i, (char*)dest + (i * SD_BLOCK_SIZE_BYTES));
    if (rc != 0){
      return rc;
    }
  }
  return 0;
}

int sd_write_block(int block_num, void* src){
  sd_clear_status();
  sd_write_arg_reg(DMA_MEM_REG, (int)src);
  sd_write_arg_reg(DMA_BLOCK_REG, block_num);
  sd_write_arg_reg(DMA_LEN_REG, SD_DMA_BLOCKS_PER_TRANSFER);
  *DMA_CTRL_REG = SD_DMA_CTRL_START | SD_DMA_CTRL_DIR_RAM_TO_SD;
  return sd_wait_done();
}

int sd_write_blocks(int start_block, int num_blocks, void* src){
  for (int i = 0; i < num_blocks; i++){
    int rc = sd_write_block(start_block + i, (char*)src + (i * SD_BLOCK_SIZE_BYTES));
    if (rc != 0){
      return rc;
    }
  }
  return 0;
}
