# add, div, and mod can probably be more efficient using shifts
# shl and shr should check if second parameter is >16 and return 0 if so
# but im lazy and this works for now
	
# args passed in r1 and r2
# result returned in r1
  .align 4
  .text
	.global smul
smul:
	# Signed multiply using shift/add on absolute values.
	# r1, r2 are inputs; r1 is return value. Uses r3-r7 as temporaries.
	lui r3 0x80000000
	mov r4 r0
	and r0 r1 r3
	bz  smul_check_r2
	add r4 r4 1
	sub r1 r0 r1
smul_check_r2:
	and r0 r2 r3
	bz  smul_abs
	add r4 r4 1
	sub r2 r0 r2
smul_abs:
	mov r3 r0
	mov r5 r2
	mov r6 r1
smul_loop:
	cmp r5 r0
	bz  smul_done
	and r7 r5 1
	bz  smul_skip_add
	add r3 r3 r6
smul_skip_add:
	lsl r6 r6 1
	lsr r5 r5 1
	jmp smul_loop
smul_done:
	add r4 r4 -1
	bnz smul_skip_negate
	sub r3 r0 r3
smul_skip_negate:
	mov r1 r3
	ret 

	.global sdiv
sdiv:
	# Signed divide: r1 / r2 -> r1 (truncates toward zero).
	# Uses unsigned long division on absolute values, then fixes the sign.
	# Division by zero is treated as implementation-defined and returns 0.
	cmp r2 r0
	bz  sdiv_divzero
	lui r3 0x80000000
	mov r4 r0
	and r0 r1 r3
	bz  sdiv_check_r2
	add r4 r4 1
	sub r1 r0 r1
sdiv_check_r2:
	and r0 r2 r3
	bz  sdiv_abs
	add r4 r4 1
	sub r2 r0 r2
sdiv_abs:
	mov r3 r0
	mov r5 r2
	mov r6 r1
	mov r7 r0
sdiv_align:
	cmp r5 r0
	bs  sdiv_loop
	lsl r1 r5 1
	cmp r1 r6
	bbe sdiv_shift
	jmp sdiv_loop
sdiv_shift:
	mov r5 r1
	add r7 r7 1
	jmp sdiv_align
sdiv_loop:
	cmp r7 r0
	bs  sdiv_done
	cmp r6 r5
	bb  sdiv_skip_sub
	sub r6 r6 r5
	mov r1 r0
	add r1 r1 1
	lsl r1 r1 r7
	or  r3 r3 r1
sdiv_skip_sub:
	lsr r5 r5 1
	add r7 r7 -1
	jmp sdiv_loop
sdiv_done:
	mov r1 r3
	add r4 r4 -1
	bnz sdiv_skip_negate
	sub r1 r0 r1
sdiv_skip_negate:
	ret
sdiv_divzero:
	mov r1 r0
	ret

	.global smod
smod:
	# Signed modulo: r1 % r2 -> r1 (remainder keeps dividend sign).
	# Uses unsigned long division on absolute values, then fixes the sign.
	# Division by zero is treated as implementation-defined and returns 0.
	cmp r2 r0
	bz  smod_divzero
	lui r3 0x80000000
	mov r8 r0
	and r0 r1 r3
	bz  smod_check_r2
	add r8 r8 1
	sub r1 r0 r1
smod_check_r2:
	and r0 r2 r3
	bz  smod_abs
	sub r2 r0 r2
smod_abs:
	mov r4 r2
	mov r5 r1
	mov r6 r0
smod_align:
	cmp r4 r0
	bs  smod_loop
	lsl r7 r4 1
	cmp r7 r5
	bbe smod_shift
	jmp smod_loop
smod_shift:
	mov r4 r7
	add r6 r6 1
	jmp smod_align
smod_loop:
	cmp r6 r0
	bs  smod_done
	cmp r5 r4
	bb  smod_skip_sub
	sub r5 r5 r4
smod_skip_sub:
	lsr r4 r4 1
	add r6 r6 -1
	jmp smod_loop
smod_done:
	mov r1 r5
	add r8 r8 -1
	bnz smod_skip_negate
	sub r1 r0 r1
smod_skip_negate:
	ret
smod_divzero:
	mov r1 r0
	ret

	.global umul
umul:
	# Unsigned multiply using shift/add.
	mov r3 r0
	mov r5 r2
	mov r6 r1
umul_loop:
	cmp r5 r0
	bz umul_end
	and r7 r5 1
	bz umul_skip_add
	add r3 r3 r6
umul_skip_add:
	lsl r6 r6 1
	lsr r5 r5 1
	jmp umul_loop
umul_end:
	mov r1 r3
	ret 

	.global udiv
udiv:
	# Unsigned divide: r1 / r2 -> r1.
	# Uses binary long division to avoid repeated subtraction.
	# Division by zero is treated as implementation-defined and returns 0.
	cmp r2 r0
	bz  udiv_divzero
	mov r3 r0
	mov r4 r2
	mov r5 r1
	mov r6 r0
udiv_align:
	cmp r4 r0
	bs  udiv_loop
	lsl r7 r4 1
	cmp r7 r5
	bbe udiv_shift
	jmp udiv_loop
udiv_shift:
	mov r4 r7
	add r6 r6 1
	jmp udiv_align
udiv_loop:
	cmp r6 r0
	bs  udiv_done
	cmp r5 r4
	bb  udiv_skip_sub
	sub r5 r5 r4
	mov r7 r0
	add r7 r7 1
	lsl r7 r7 r6
	or  r3 r3 r7
udiv_skip_sub:
	lsr r4 r4 1
	add r6 r6 -1
	jmp udiv_loop
udiv_done:
	mov r1 r3
	ret
udiv_divzero:
	mov r1 r0
	ret

	.global umod
umod:
	# Unsigned modulo: r1 % r2 -> r1.
	# Uses binary long division to avoid repeated subtraction.
	# Division by zero is treated as implementation-defined and returns 0.
	cmp r2 r0
	bz  umod_divzero
	mov r4 r2
	mov r5 r1
	mov r6 r0
umod_align:
	cmp r4 r0
	bs  umod_loop
	lsl r7 r4 1
	cmp r7 r5
	bbe umod_shift
	jmp umod_loop
umod_shift:
	mov r4 r7
	add r6 r6 1
	jmp umod_align
umod_loop:
	cmp r6 r0
	bs  umod_done
	cmp r5 r4
	bb  umod_skip_sub
	sub r5 r5 r4
umod_skip_sub:
	lsr r4 r4 1
	add r6 r6 -1
	jmp umod_loop
umod_done:
	mov r1 r5
	ret
umod_divzero:
	mov r1 r0
	ret
	
	.global sleft_shift
sleft_shift:
	# check sign of r2
	# if negative, do right shift instead
	lui r3 0x80000000
	and r0 r3 r2
	bz  sls_loop
	sub r2 r0 r2
	jmp srs_loop
sls_loop: # repeated shift
	cmp r2 r0
	bz  sls_end
	add r2 r2 -1
	lsl r1 r1 1
	jmp sls_loop
sls_end:
	ret
	
	.global sright_shift
sright_shift:
	# check sign of r2
	# if negative, do left shift instead
	lui r3 0x80000000
	and r0 r3 r2
	bz  srs_loop
	sub r2 r0 r2
	jmp sls_loop
srs_loop: # repeated shift
	cmp r2 r0
	bz  srs_end
	add r2 r2 -1
	asr r1 r1 1
	jmp srs_loop
srs_end:
	ret

	.global uleft_shift
uleft_shift:
	# check sign of r2
	# if negative, do right shift instead
	lui r3 0x80000000
	and r0 r3 r2
	bz  uls_loop
	sub r2 r0 r2
	jmp urs_loop
uls_loop: # repeated shift
	cmp r2 r0
	bz  uls_end
	add r2 r2 -1
	lsl r1 r1 1
	jmp uls_loop
uls_end:
	ret
	
	.global uright_shift
uright_shift:
	# check sign of r2
	# if negative, do left shift instead
	lui r3 0x80000000
	and r0 r3 r2
	bz  urs_loop
	sub r2 r0 r2
	jmp uls_loop
urs_loop: # repeated shift
	cmp r2 r0
	bz  urs_end
	add r2 r2 -1
	lsr r1 r1 1
	jmp urs_loop
urs_end:
	ret
