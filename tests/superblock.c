/*
 * Superblock dump test.
 *
 * Validates:
 * - the SD driver can read the ext2 superblock blocks from drive 1
 * - the in-memory Superblock layout matches the bytes stored on disk closely
 *   enough to decode the core fields printed here
 *
 * How:
 * - read the two blocks that contain the ext2 superblock into one Superblock
 *   instance
 * - print the key geometry and policy fields, then decode the enum-style fields
 *   into readable names where possible
 */

#include "../kernel/debug.h"
#include "../kernel/sd_driver.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/ext.h"

struct Superblock superblock;

// Read the ext2 superblock from drive 1 and print its decoded fields.
int kernel_main(void){
  int err = sd_read_blocks(SD_DRIVE_1, 2, 2, &superblock);
  assert(err == 0, "superblock test: sd_read_blocks failed.\n");

  sizeof(struct Superblock);

  // Print the raw count and geometry fields first.
  say("***Superblock contents:\n", NULL);
  say("***inodes_count: %d\n", (int*)&superblock.inodes_count);
  say("***blocks_count: %d\n", (int*)&superblock.blocks_count);
  say("***r_blocks_count: %d\n", (int*)&superblock.r_blocks_count);
  say("***free_blocks_count: %d\n", (int*)&superblock.free_blocks_count);
  say("***free_inodes_count: %d\n", (int*)&superblock.free_inodes_count);
  say("***first_data_block: %d\n", (int*)&superblock.first_data_block);
  say("***log_block_size: %d\n", (int*)&superblock.log_block_size);
  say("***log_frag_size: %d\n", (int*)&superblock.log_frag_size);
  say("***blocks_per_group: %d\n", (int*)&superblock.blocks_per_group);
  say("***frags_per_group: %d\n", (int*)&superblock.frags_per_group);
  say("***inodes_per_group: %d\n", (int*)&superblock.inodes_per_group);

  int mnt_count = superblock.mnt_count;
  say("***mnt_count: %d\n", &mnt_count);
  int magic = superblock.magic;
  say("***magic: %X\n", &magic);
  int state = superblock.state;

  // Decode the small enum-style fields into names to keep the output readable.
  say("***state: ", NULL);
  if (state == EXT2_VALID_FS) {
    say("valid\n", NULL);
  } else if (state == EXT2_ERROR_FS) {
    say("error\n", NULL);
  } else {
    say("unknown state %d\n", &state);
  }

  int errors = superblock.errors;
  say("***errors: ", NULL);
  if (errors == EXT2_ERRORS_CONTINUE) {
    say("continue\n", NULL);
  } else if (errors == EXT2_ERRORS_RO) {
    say("remount read-only\n", NULL);
  } else if (errors == EXT2_ERRORS_PANIC) {
    say("panic\n", NULL);
  } else {
    say("unknown error behavior %d\n", &errors);
  }

  int creator_os = superblock.creator_os;
  say("***creator_os: ", NULL);
  if (creator_os == EXT2_OS_LINUX) {
    say("linux\n", NULL);
  } else if (creator_os == EXT2_OS_HURD) {
    say("hurd\n", NULL);
  } else if (creator_os == EXT2_OS_MASIX) {
    say("masix\n", NULL);
  } else if (creator_os == EXT2_OS_FREEBSD) {
    say("freebsd\n", NULL);
  } else if (creator_os == EXT2_OS_LITES) {
    say("lites\n", NULL);
  } else if (creator_os == EXT2_OS_DIOPTASE) {
    say("dioptase\n", NULL);
  } else {
    say("unknown creator OS %d\n", &creator_os);
  }

  say("***rev_level: %d\n", (int*)&superblock.rev_level);
  say("***first_ino: %d\n", (int*)&superblock.first_ino);

  int inode_size = superblock.inode_size;
  say("***inode_size: %d\n", &inode_size);
  return 0;
}
