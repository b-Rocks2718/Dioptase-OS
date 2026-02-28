# Kernel Memory Map

### 0x0000000 - 0x00003FF
Interrupt Vector Table

### 0x400 - ...
Where BIOS code is loaded (32KB reserved). Can overwrite once kernel is entered. 

### 0x10000 - 0x90000
Kernel text (512KB reserved for now)

### 0x90000 - 0xA0000
Kernel data (64KB)

### 0xA0000 - 0xB0000
Kernel rodata (64KB)

### 0xB0000 - 0xF0000
Kernel bss (256KB)

### 0xF0000 - 0x100000
Kernel Stacks (16KB each)   
- Core 3: 0xF0000 - 0xF4000   
- Core 2: 0xF4000 - 0xF8000   
- Core 1: 0xF8000 - 0xFC000   
- Core 0: 0xFC000 - 0x100000   

### 0x100000 - 0x800000
Kernel Heap (7MB)  

### 0x800000 - ...
VMM frames to allocate (~120MB of frames)

### 0x7FC0000 - 0x7FFFFFF
I/O devices - see [memory map](https://github.com/b-Rocks2718/Dioptase/blob/main/docs/mem_map.md)
