#include "sys.h"
#include "interrupts.h"
#include "debug.h"
#include "print.h"
#include "ivt.h"
#include "constants.h"
#include "vmem.h"
#include "elf.h"

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
    case TRAP_CODE_EXIT: {
      // return instead to the kernel thread that called switch_to_user
      *return_to_user = false;
      return arg1;
    }
    case TRAP_CODE_TEST_SYSCALL: {
      return trap_test_syscall_handler(arg1);
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
