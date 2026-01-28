
	.text
# add, div, and mod can probably be more efficient using shifts
# shl and shr should check if second parameter is >16 and return 0 if so
# but im lazy and this works for now

# ---- Constants for CRT memory helpers ----
# HEAP_ALIGN_BYTES: Heap allocations are rounded up to 4 bytes for word alignment.
# HEAP_ALIGN_MASK: Mask for alignment (HEAP_ALIGN_BYTES - 1).
# HEAP_ALIGN_NEG_MASK: Bitwise mask for clearing low alignment bits.
# HEAP_SIZE_BYTES: Size of the CRT bump-allocator heap in bytes.
# BYTE_STRIDE: Per-byte increment used for byte-wise loops.
# DEC_ONE: Decrement constant for loop counters.
# BYTE_MASK: Mask to zero-extend byte loads for memcmp.
	.define HEAP_ALIGN_BYTES 4
	.define HEAP_ALIGN_MASK 3
	.define HEAP_ALIGN_NEG_MASK -4
	.define HEAP_SIZE_BYTES 65536
	.define BYTE_STRIDE 1
	.define DEC_ONE -1
	.define BYTE_MASK 0xFF
	
# args passed in r1 and r2
# result returned in r1
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

	.global strcmp
strcmp:
	# Purpose: Compare two NUL-terminated byte strings lexicographically.
	# Inputs: r1 = s1 (pointer to first string), r2 = s2 (pointer to second string).
	# Outputs: r1 = signed difference of first mismatched bytes (0 if equal).
	# Preconditions: s1 and s2 point to readable, NUL-terminated byte strings.
	# Postconditions: r1 < 0 if s1 < s2, r1 == 0 if s1 == s2, r1 > 0 if s1 > s2.
	# Invariants: r0 remains zero; memory is read-only; no stack usage.
	# CPU state assumptions: executes in caller mode; interrupts and MMU state
	# are unchanged and must allow reads of s1/s2; core count is irrelevant but
	# callers must avoid concurrent mutation of the string data.
strcmp_loop:
	# Load current bytes (zero-extended) from each string.
	lba r3 [r1]
	lba r4 [r2]
	# If bytes differ, return their signed difference.
	cmp r3 r4
	bz  strcmp_check_end
	sub r1 r3 r4
	ret
strcmp_check_end:
	# If both bytes are NUL, strings match.
	cmp r3 r0
	bz  strcmp_equal
	# Advance to next byte and continue.
	add r1 r1 1
	add r2 r2 1
	jmp strcmp_loop
strcmp_equal:
	mov r1 r0
	ret

	.global malloc
malloc:
	# Purpose: Simple bump allocator for the CRT heap.
	# Inputs: r1 = size in bytes (low 32 bits used).
	# Outputs: r1 = pointer to allocated block, or 0 on failure/size==0.
	# Preconditions: __heap_ptr is a word in .bss; __heap_base/__heap_end bound a
	# heap region. Caller must serialize allocations (not thread-safe).
	# Postconditions: On success, __heap_ptr advances by aligned size.
	# Invariants: r0 remains zero; heap pointer stays HEAP_ALIGN_BYTES-aligned.
	# CPU state assumptions: Executes in caller mode; interrupts/MMU unchanged;
	# no concurrent heap mutation across cores.
	cmp r1 r0
	bz  malloc_return_zero

	# Align size: size = (size + HEAP_ALIGN_MASK) & HEAP_ALIGN_NEG_MASK
	mov r2 r1
	add r2 r2 HEAP_ALIGN_MASK
	movi r3 HEAP_ALIGN_NEG_MASK
	and r2 r2 r3

	# Load __heap_ptr into r3.
	movi r4 __heap_ptr
	br r5 r0
	add r4 r4 r5
	lwa r3 [r4, 0]

	# Initialize heap pointer on first use.
	cmp r3 r0
	bnz malloc_have_ptr
	movi r6 __heap_base
	br r5 r0
	add r6 r6 r5
	mov r3 r6
	swa r3 [r4, 0]
malloc_have_ptr:
	# Compute new_ptr = heap_ptr + aligned_size.
	add r6 r3 r2

	# Load heap end address.
	movi r7 __heap_end
	br r5 r0
	add r7 r7 r5

	# Fail if new_ptr > __heap_end (unsigned compare).
	cmp r6 r7
	ba  malloc_fail

	# Commit allocation and return old heap pointer.
	swa r6 [r4, 0]
	mov r1 r3
	ret
malloc_fail:
	mov r1 r0
	ret
malloc_return_zero:
	mov r1 r0
	ret

	.global calloc
calloc:
	# Purpose: Allocate and zero-initialize nmemb * size bytes.
	# Inputs: r1 = nmemb, r2 = size (low 32 bits used).
	# Outputs: r1 = pointer to zeroed block, or 0 on failure/zero size.
	# Preconditions: malloc is available; heap state is initialized as needed.
	# Postconditions: On success, returned block is zero-filled.
	# Invariants: r0 remains zero; heap pointer remains aligned.
	# CPU state assumptions: Executes in caller mode; interrupts/MMU unchanged;
	# no concurrent heap mutation across cores.
	# Saves/Restores: ra is saved on the stack because calloc calls helpers.
	push ra
	cmp r1 r0
	bz  calloc_return_zero
	cmp r2 r0
	bz  calloc_return_zero

	# total = nmemb * size
	call umul
	cmp r1 r0
	bz  calloc_return_zero

	# Preserve total across malloc using a caller-saved register to avoid stack.
	mov r8 r1
	call malloc
	mov r2 r8
	cmp r1 r0
	bz  calloc_return

	# Zero-fill the allocation.
	mov r3 r1
	mov r4 r2
calloc_zero_loop:
	cmp r4 r0
	bz  calloc_done
	sba r0 [r3, 0]
	add r3 r3 BYTE_STRIDE
	add r4 r4 DEC_ONE
	jmp calloc_zero_loop
calloc_done:
	jmp calloc_return
calloc_return_zero:
	mov r1 r0
calloc_return:
	pop ra
	ret

	.global memcmp
memcmp:
	# Purpose: Compare two byte arrays lexicographically.
	# Inputs: r1 = s1 pointer, r2 = s2 pointer, r3 = length in bytes.
	# Outputs: r1 = 0 if equal, <0 if s1<s2, >0 if s1>s2.
	# Preconditions: s1/s2 are valid for r3 bytes; buffers may overlap.
	# Postconditions: r1 holds the first byte difference (unsigned compare).
	# Invariants: r0 remains zero; memory is read-only.
	# CPU state assumptions: Executes in caller mode; interrupts/MMU unchanged;
	# no concurrent mutation of compared memory.
	cmp r3 r0
	bz  memcmp_equal
	movi r6 BYTE_MASK
memcmp_loop:
	lba r4 [r1, 0]
	lba r5 [r2, 0]
	and r4 r4 r6
	and r5 r5 r6
	cmp r4 r5
	bz  memcmp_next
	sub r1 r4 r5
	ret
memcmp_next:
	add r1 r1 BYTE_STRIDE
	add r2 r2 BYTE_STRIDE
	add r3 r3 DEC_ONE
	bnz memcmp_loop
memcmp_equal:
	mov r1 r0
	ret

	.data
	.align HEAP_ALIGN_BYTES
# Purpose: Heap storage for malloc/calloc bump allocator.
# Address range: __heap_base .. __heap_end (exclusive).
# Side effects: malloc/calloc update __heap_ptr; no reuse or free support.
# Timing/ordering: Allocations must be serialized; no concurrent mutation.
__heap_ptr:
	.fill 0
__heap_base:
	.space HEAP_SIZE_BYTES
__heap_end:
