#include "sys.h"
#include "interrupts.h"
#include "debug.h"
#include "print.h"
#include "ivt.h"

// handle an exit syscall
static unsigned trap_exit_handler(int rc){
  say("trap: exit requested, return code = %d\n", &rc);
  panic("trap: halting after user exit.\n");
}

static unsigned unrecognized_trap_handler(unsigned code){
  say("trap: unrecognized trap code = %d\n", &code);
  panic("trap: halting due to unrecognized trap.\n");
}

// Dispatch user-mode trap requests after trap_handler_ has preserved
// the hardware trap frame and switched into the kernel C calling convention
void trap_handler(unsigned code, 
    int arg1, int arg2, int arg3, int arg4, int arg5, int arg6, int arg7){
  switch (code){
    case TRAP_CODE_EXIT: {
      trap_exit_handler(arg1);
      break;
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
