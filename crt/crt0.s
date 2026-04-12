	.text
	
	.global _start
_start:
  # eventually heap init would be called here
	call main

  mov  r2, r1
_start_exit_loop:
  movi r1, 0
  trap
  jmp _start_exit_loop

.global exit
exit:
  mov  r2, r1
exit_loop:
  movi r1, 0
  trap
  jmp exit_loop
  
