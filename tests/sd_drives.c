#include "../kernel/debug.h"
#include "../kernel/sd_driver.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"

void show_block(enum SdDrive drive, unsigned block){
  char* buf_0 = malloc(512);
  sd_read_block(drive, block, buf_0);
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

int kernel_main(void){
  say("***Testing SD drive reads...\n", NULL);
  show_block(SD_DRIVE_0, 0);
  show_block(SD_DRIVE_1, 2);
}