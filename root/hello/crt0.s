	.text
  .align 4
	
	.global _start
_start:

	call main

_start_exit_loop:
  mov r1, r0
  trap
  jmp _start_exit_loop

  .global write
write:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r4, r3
  mov  r3, r2
  mov  r2, r1
  movi r1, 16
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret
  