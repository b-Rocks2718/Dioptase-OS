	
	.data
	.align 1
string.label.1:
	.filb 72
	.filb 101
	.filb 108
	.filb 108
	.filb 111
	.filb 44
	.filb 32
	.filb 87
	.filb 111
	.filb 114
	.filb 108
	.filb 100
	.filb 33
	.filb 10
	.space 1
	
	.text

	.global main
main:
	# Function Prologue
	push ra
	push bp
	mov bp sp
	# Function Body
	mov r9 sp
	movi r10 8
	sub r9 r9 r10
	mov sp r9
	movi r10 string.label.1
	br r9, r0
	add r10 r10 r9
	swa r10, [bp, -4]
	lwa r1, [bp, -4]
	call puts
	swa r1, [bp, -8]
	movi r1 0
	# Function Epilogue
	mov sp bp
	lwa ra, [bp, 4]
	lwa bp, [bp, 0]
	add sp sp 8
	ret
	movi r1 0
	# Function Epilogue
	mov sp bp
	lwa ra, [bp, 4]
	lwa bp, [bp, 0]
	add sp sp 8
	ret
