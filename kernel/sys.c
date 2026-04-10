#include "sys.h"
#include "interrupts.h"
#include "debug.h"
#include "print.h"

void sys_exit_handler(unsigned rc){
  say("got sys exit, return code = %d\n", &rc);

  panic("halting\n");
}

void sys_init(void) {
  register_handler((void*)sys_exit_handler, (void*)0x004);
}