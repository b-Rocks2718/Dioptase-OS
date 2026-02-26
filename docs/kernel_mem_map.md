# Kernel Memory Map

### 0x0000000 - 0x00003FF
Interrupt Vector Table

### 0x400
Where BIOS code is loaded. Can overwrite once kernel is entered. 

### 0x5000
BIOS stack top

### 0x5000 - 0x19000
Kernel text 

### 0x19000 - 0x1B000
Kernel data

### 0x1B000 - 0x1B000
Kernel rodata

### 0x1B000 - 0x1B000
Kernel bss 

### 0x1B000 - 0x1D000
Kernel Stacks    
- Core 0: 0x1B000 - 0x1D000   

### 0x1D000 - 0x20000
Kernel Heap

### 0x7FC0000 - 0x7FFFFFF
I/O devices - see [memory map](https://github.com/b-Rocks2718/Dioptase/blob/main/docs/mem_map.md)
