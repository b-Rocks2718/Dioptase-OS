	.text
	
	.global _start
_start:
  # eventually heap init would be called here
	call main

loop:
	sys EXIT
	jmp loop

.global exit
exit:
  sys EXIT
  jmp exit