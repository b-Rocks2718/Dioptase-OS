	
	.data
	
	.text

write_fd_all:
	# Function Prologue
	push ra
	push bp
	mov bp sp
	# Function Body
	mov r9 sp
	movi r10 32
	sub r9 r9 r10
	mov sp r9
	swa r1, [bp, -4]
	swa r2, [bp, -8]
	swa r3, [bp, -12]
write_fd_all.while.13.continue:
	movi r9 1
	swa r9, [bp, -16]
	lwa r9, [bp, -12]
	movi r10 0
	cmp r9 r10
	bnz 4
	jmp 12
	movi r10 write_fd_all.end.2
	br r0, r10
	movi r9 0
	swa r9, [bp, -16]
write_fd_all.end.2:
	lwa r9, [bp, -16]
	movi r10 0
	cmp r9 r10
	bz 4
	jmp 12
	movi r10 write_fd_all.while.13.break
	br r0, r10
	lwa r1, [bp, -4]
	lwa r2, [bp, -8]
	lwa r3, [bp, -12]
	call write
	swa r1, [bp, -20]
	lwa r9, [bp, -20]
	swa r9, [bp, -24]
	movi r9 1
	swa r9, [bp, -28]
	lwa r9, [bp, -24]
	movi r10 0
	cmp r9 r10
	ble 4
	jmp 12
	movi r10 write_fd_all.end.0
	br r0, r10
	movi r9 0
	swa r9, [bp, -28]
write_fd_all.end.0:
	lwa r9, [bp, -28]
	movi r10 0
	cmp r9 r10
	bz 4
	jmp 12
	movi r10 write_fd_all.end.1
	br r0, r10
	# Function Epilogue
	mov sp bp
	lwa ra, [bp, 4]
	lwa bp, [bp, 0]
	add sp sp 8
	ret
write_fd_all.end.1:
	lwa r9, [bp, -24]
	movi r10 1
	mov r1 r9
	mov r2 r10
	call umul
	mov r9 r1
	swa r9, [bp, -32]
	lwa r9, [bp, -8]
	lwa r10, [bp, -32]
	add r9 r9 r10
	swa r9, [bp, -8]
	lwa r9, [bp, -12]
	lwa r10, [bp, -24]
	sub r9 r9 r10
	swa r9, [bp, -12]
	movi r10 write_fd_all.while.13.continue
	br r0, r10
write_fd_all.while.13.break:
	movi r1 0
	# Function Epilogue
	mov sp bp
	lwa ra, [bp, 4]
	lwa bp, [bp, 0]
	add sp sp 8
	ret

	.global fdputs
fdputs:
	# Function Prologue
	push ra
	push bp
	mov bp sp
	# Function Body
	mov r9 sp
	movi r10 24
	sub r9 r9 r10
	mov sp r9
	swa r1, [bp, -4]
	swa r2, [bp, -8]
	lwa r9, [bp, -8]
	swa r9, [bp, -12]
	movi r9 0
	swa r9, [bp, -16]
fdputs.while.14.continue:
	lwa r9, [bp, -8]
	mov r10 r9
	lba r9, [r10, 0]
	sba r9, [bp, -17]
	lba r9, [bp, -17]
	movi r10 0
	cmp r9 r10
	bz 4
	jmp 12
	movi r10 fdputs.while.14.break
	br r0, r10
	movi r9 1
	movi r10 1
	mov r1 r9
	mov r2 r10
	call smul
	mov r9 r1
	swa r9, [bp, -24]
	lwa r9, [bp, -8]
	lwa r10, [bp, -24]
	add r9 r9 r10
	swa r9, [bp, -8]
	lwa r9, [bp, -16]
	movi r10 1
	add r9 r9 r10
	swa r9, [bp, -16]
	movi r10 fdputs.while.14.continue
	br r0, r10
fdputs.while.14.break:
	lwa r1, [bp, -4]
	lwa r2, [bp, -12]
	lwa r3, [bp, -16]
	call write_fd_all
	lwa r1, [bp, -16]
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

	.global puts
puts:
	# Function Prologue
	push ra
	push bp
	mov bp sp
	# Function Body
	mov r9 sp
	movi r10 8
	sub r9 r9 r10
	mov sp r9
	swa r1, [bp, -4]
	movi r1 1
	lwa r2, [bp, -4]
	call fdputs
	swa r1, [bp, -8]
	lwa r1, [bp, -8]
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
