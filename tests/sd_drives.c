/*
 * SD drive read test.
 *
 * Validates:
 * - both SD drive MMIO paths can read one 512-byte block through the shared
 *   kernel sd_driver API
 * - the multi-drive API routes drive 0 and drive 1 requests independently
 *
 * How:
 * - read one block from drive 0 and one block from drive 1
 * - dump each 512-byte payload in hex so the test log shows which drive and
 *   block were read and what bytes came back
 */

#include "../kernel/debug.h"
#include "../kernel/sd_driver.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"

// Read one block from one drive and print it as 16-byte hex rows.
void show_block(enum SdDrive drive, unsigned block){
  char* buf_0 = malloc(512);
  assert(buf_0 != NULL, "sd_drives: block buffer allocation failed.\n");
  int err = sd_read_blocks(drive, block, 1, buf_0);
  assert(err == 0, "sd_drives: sd_read_blocks failed.\n");
  int args[2] = {(int)drive, (int)block};
  say("***SD drive %d, block %d contents:\n***", args);
  for (int i = 0; i < 512; i++) {
    int c = buf_0[i];
    c &= 0xFF;
    say("%X ", &c);
    if ((i + 1) % 16 == 0) {
      say("\n***", NULL);
    }
  }
  say("\n", NULL);
  free(buf_0);
}

// Probe both configured SD drives with one representative block read each.
int kernel_main(void){
  say("***Testing SD drive reads...\n", NULL);
  show_block(SD_DRIVE_0, 0);
  show_block(SD_DRIVE_1, 2);
  return 0;
}
