#include "exc.h"
#include "ivt.h"
#include "debug.h"
#include "machine.h"
#include "print.h"
#include "sys.h"
#include "threads.h"

void exc_init(void){
  register_handler((void*)invalid_instr_exc_handler_, (void*)INVALID_INSTR_IVT_ENTRY);
  register_handler((void*)priv_exc_handler_, (void*)PRIV_EXC_IVT_ENTRY);
  register_handler((void*)misaligned_pc_exc_handler_, (void*)MISALIGNED_PC_IVT_ENTRY);
}

int invalid_instr_handler(bool* return_to_user, unsigned epc){
  bool was_user = (get_cr0() == 1);

  if (was_user){
    // The assembly exception wrapper retains the faulting EPC/EFG beneath this
    // C activation. sigreturn() returns here and the wrapper restores that
    // exact context, so delivery retries the invalid instruction.
    if (try_run_current_signal_handler(SIGNAL_ILL, epc, 0)){
      return 0;
    }

    int args[1] = {(int)epc};
    say("| exception: user program killed by invalid instruction at epc=0x%X\n",
      args);
    *return_to_user = false;
    return -1;
  }

  panic("Invalid instruction exception\n");
}

int priv_instr_handler(bool* return_to_user, unsigned epc){
  bool was_user = (get_cr0() == 1);

  if (was_user){
    // SIGNAL_ILL covers both invalid encodings and instructions that are valid
    // only in kernel mode. As with invalid instructions, sigreturn() restores
    // the saved exception frame and retries this exact EPC.
    if (try_run_current_signal_handler(SIGNAL_ILL, epc, 0)){
      return 0;
    }

    int args[1] = {(int)epc};
    say("| exception: user program killed by privileged instruction at epc=0x%X\n",
      args);
    *return_to_user = false;
    return -1;
  }

  panic("Privileged instruction exception\n");
}

int misaligned_pc_handler(bool* return_to_user, unsigned epc){
  bool was_user = (get_cr0() == 1);

  if (was_user){
    if (try_run_current_signal_handler(SIGNAL_ALGN, epc, 0)){
      return 0;
    }

    int args[1] = {(int)epc};
    say("| exception: user program killed by misaligned pc=0x%X\n", args);
    *return_to_user = false;
    return -1;
  }

  say("| misaligned pc epc=0x%X\n", &epc);
  panic("Misaligned PC exception\n");
}
