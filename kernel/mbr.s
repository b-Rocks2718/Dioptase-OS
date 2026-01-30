  .origin 0x00000000
.fill KERNEL_START_BLOCK # First word is block offset where kernel code starts
.fill KERNEL_NUM_BLOCKS # Second word is number of blocks occupied by kernel code
.fill KERNEL_LOAD_ADDR # Third word is physical load address of kernel
.space 498 # Pad to 510 bytes
.fild 0xAA55 # Boot signature
