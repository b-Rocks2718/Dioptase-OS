	.text
  .align 4
	
	.global _start
_start:
  # save argc and argv
  mov  r20, r1
  mov  r21, r2

  movi r1, 0x10000
  call heap_init

  # pass argc and argv to main
  mov  r1, r20
  mov  r2, r21

	call main

_start_exit_loop:
  call exit
  jmp _start_exit_loop

  