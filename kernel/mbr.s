  .origin 0x00000000
.fill TEXT_START_BLOCK # First word is block offset where kernel code starts
.fill TEXT_NUM_BLOCKS # Second word is number of blocks occupied by kernel code
.fill TEXT_LOAD_ADDR # Third word is physical load address of kernel
.fill DATA_START_BLOCK # First word is block offset where kernel code starts
.fill DATA_NUM_BLOCKS # Second word is number of blocks occupied by kernel code
.fill DATA_LOAD_ADDR # Third word is physical load address of kernel
.fill RODATA_START_BLOCK # First word is block offset where kernel code starts
.fill RODATA_NUM_BLOCKS # Second word is number of blocks occupied by kernel code
.fill RODATA_LOAD_ADDR # Third word is physical load address of kernel
.fill BSS_NUM_BLOCKS # Second word is number of blocks occupied by kernel code
.fill BSS_LOAD_ADDR # Third word is physical load address of kernel
.space 466 # Pad to 510 bytes
.fild 0xAA55 # Boot signature
