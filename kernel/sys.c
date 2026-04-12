#include "sys.h"
#include "interrupts.h"
#include "debug.h"
#include "print.h"
#include "ivt.h"
#include "constants.h"
#include "vmem.h"
#include "elf.h"
#include "pit.h"
#include "vga.h"
#include "ps2.h"
#include "threads.h"

#define INITIAL_USER_STACK_SIZE 0x4000

static unsigned trap_test_syscall_handler(int arg){
  say("***test_syscall arg = %d\n", &arg);
  return arg + 7;
}

static unsigned unrecognized_trap_handler(unsigned code){
  say("| trap: unrecognized trap code = %d\n", &code);
  panic("trap: halting due to unrecognized trap.\n");
  return 0;
}

// Dispatch user-mode trap requests after trap_handler_ has preserved
// the hardware trap frame and switched into the kernel C calling convention
int trap_handler(unsigned code,
    int arg1, int arg2, int arg3, int arg4, int arg5, int arg6, int arg7,
    bool* return_to_user){

  // most sycalls return to the user program
  *return_to_user = true;

  switch (code){
    case TRAP_EXIT: {
      // return instead to the kernel thread that called switch_to_user
      *return_to_user = false;
      return arg1;
    }
    case TRAP_TEST_SYSCALL: {
      return trap_test_syscall_handler(arg1);
    }
    case TRAP_GET_CURRENT_JIFFIES: {
      return current_jiffies;
    }
    case TRAP_GET_KEY: {
      return getkey();
    }
    case TRAP_SET_TILE_SCALE: {
      *TILE_SCALE = arg1;
      return 0;
    }
    case TRAP_SET_VSCROLL: {
      *TILE_VSCROLL = arg1;
      return 0;
    }
    case TRAP_SET_HSCROLL: {
      *TILE_HSCROLL = arg1;
      return 0;
    }
    case TRAP_LOAD_TEXT_TILES: {
      load_text_tiles();
      return 0;
    }
    case TRAP_CLEAR_SCREEN: {
      clear_screen();
      return 0;
    }
    case TRAP_GET_TILEMAP: {
      return (int)mmap_physmem(TILEMAP_SIZE, (unsigned)TILEMAP, MMAP_READ | MMAP_WRITE | MMAP_USER);
    }
    case TRAP_GET_TILE_FB: {
      return (int)mmap_physmem(TILE_FB_SIZE, (unsigned)TILE_FB, MMAP_READ | MMAP_WRITE | MMAP_USER);
    }
    case TRAP_GET_VGA_STATUS: {
      return (unsigned char)(*VGA_STATUS);
    }
    case TRAP_GET_VGA_FRAME_COUNTER: {
      return *VGA_FRAME_COUNTER;
    }
    case TRAP_SLEEP: {
      sleep(arg1);
      return 0;
    }
    default: {
      unrecognized_trap_handler(code);
      break;
    }
  }
}

void trap_init(void) {
  register_handler((void*)trap_handler_, (void*)TRAP_IVT_ENTRY);
}

// run a user program given a node representing its ELF file
// consumes the node, so the caller cannot use it after calling this function
int run_user_program(struct Node* prog_node){
  unsigned size = node_size_in_bytes(prog_node);
  unsigned* prog = mmap(size, prog_node, 0, MMAP_READ);
  node_free(prog_node);
  
  unsigned entry = elf_load(prog);

  // The initial user stack grows downward, so reserve it from the top of the
  // user half and enter at the last word in that reservation.
  unsigned* stack = mmap_stack(INITIAL_USER_STACK_SIZE,
    MMAP_READ | MMAP_WRITE | MMAP_USER);

  return jump_to_user(entry,
    (unsigned)stack + INITIAL_USER_STACK_SIZE - sizeof(unsigned));
}
