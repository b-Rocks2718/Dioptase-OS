#include "exc.h"
#include "ivt.h"
#include "debug.h"

void exc_init(void){
  register_handler((void*)invalid_instr_exc_handler_, (void*)INVALID_INSTR_IVT_ENTRY);
  register_handler((void*)priv_exc_handler_, (void*)PRIV_EXC_IVT_ENTRY);
  register_handler((void*)misaligned_pc_exc_handler_, (void*)MISALIGNED_PC_IVT_ENTRY);
}

// TODO: use psr value to work out if the exception happened
// in user or kernel mode. user mode is probably recoverable,
// kernel mode probably not

void invalid_instr_handler(void){
  panic("Invalid instruction exception\n");
}

void priv_instr_handler(void){
  panic("Privileged instruction exception\n");
}

void misaligned_pc_handler(void){
  panic("Misaligned PC exception\n");
}

