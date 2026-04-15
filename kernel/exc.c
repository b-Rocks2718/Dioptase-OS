#include "exc.h"
#include "ivt.h"
#include "debug.h"
#include "machine.h"
#include "print.h"

void exc_init(void){
  register_handler((void*)invalid_instr_exc_handler_, (void*)INVALID_INSTR_IVT_ENTRY);
  register_handler((void*)priv_exc_handler_, (void*)PRIV_EXC_IVT_ENTRY);
  register_handler((void*)misaligned_pc_exc_handler_, (void*)MISALIGNED_PC_IVT_ENTRY);
}

int invalid_instr_handler(bool* return_to_user){
  bool was_user = (get_cr0() == 1);

  if (was_user){
    // User code executed an invalid instruction. Abort back to the kernel caller
    // of `jump_to_user(...)`.
    say("User program killed due to invalid instruction\n", NULL);
    *return_to_user = false;
    return -1;
  }

  panic("Invalid instruction exception\n");
}

int priv_instr_handler(bool* return_to_user){
  bool was_user = (get_cr0() == 1);

  if (was_user){
    // User code executed a privileged instruction. Abort back to the kernel caller
    // of `jump_to_user(...)`.
    say("User program killed due to privileged instruction\n", NULL);
    *return_to_user = false;
    return -1;
  }

  panic("Privileged instruction exception\n");
}

int misaligned_pc_handler(bool* return_to_user){
  bool was_user = (get_cr0() == 1);

  if (was_user){
    // User code executed a misaligned PC. Abort back to the kernel caller
    // of `jump_to_user(...)`.
    say("User program killed due to misaligned PC\n", NULL);
    *return_to_user = false;
    return -1;
  }

  panic("Misaligned PC exception\n");
}

