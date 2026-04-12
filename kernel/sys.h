#ifndef SYS_H
#define SYS_H

#include "ext.h"

enum TrapCode {
  TRAP_EXIT = 0,
  TRAP_TEST_SYSCALL = 1,
  TRAP_GET_CURRENT_JIFFIES = 2,
  TRAP_GET_KEY = 3,
  TRAP_SET_TILE_SCALE = 4,
  TRAP_SET_VSCROLL = 5,
  TRAP_SET_HSCROLL = 6,
  TRAP_LOAD_TEXT_TILES = 7,
  TRAP_CLEAR_SCREEN = 8,
  TRAP_GET_TILEMAP = 9,
  TRAP_GET_TILE_FB = 10,
  TRAP_GET_VGA_STATUS = 11,
  TRAP_GET_VGA_FRAME_COUNTER = 12,
  TRAP_SLEEP = 13,
};

// set up IVT with trap handler entry point
void trap_init(void);

// Enter user mode through rfe
unsigned jump_to_user(unsigned entry, unsigned stack);

// run a user program given a node representing its ELF file
// consumes the node, so the caller cannot use it after calling this function
int run_user_program(struct Node* prog_node);

extern void trap_handler_(void);

#endif // SYS_H
