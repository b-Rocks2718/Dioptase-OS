	.text
  .align 4
	
	.global _start
_start:
  movi r1, 0x10000
  call heap_init

	call main

_start_exit_loop:
  call exit
  jmp _start_exit_loop

  