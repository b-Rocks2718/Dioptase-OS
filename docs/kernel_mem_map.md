# Kernel Memory Map

### 0x0000000 - 0x00003FF
Interrupt Vector Table

### 0x400 - ...
Where BIOS code is loaded (64KB reserved). Can overwrite once kernel is entered. 

### 0x10000 - ...
Kernel Code (~1MB reserved for now)

### 0xE0000 - 0xF0000
Interrupt Stacks (16KB each)   
- Core 3: 0xE0000 - 0xE4000   
- Core 2: 0xE4000 - 0xE8000   
- Core 1: 0xE8000 - 0xEC000   
- Core 0: 0xEC000 - 0xF0000  

### 0xF0000 - 0x100000
Kernel Stacks (16KB each)   
- Core 3: 0xF0000 - 0xF4000   
- Core 2: 0xF4000 - 0xF8000   
- Core 1: 0xF8000 - 0xFC000   
- Core 0: 0xFC000 - 0x100000   

### 0x200000 - 0x800000
Kernel Heap (6MB)  
heap will also use like 9 bytes before 0x200000

### 0x800000 - ...
VMM frames to allocate (~120MB of frames)

### 0x7FC0000 - 0x7FFFFFF
I/O devices - see [memory map](https://github.com/b-Rocks2718/Dioptase/blob/main/docs/mem_map.md)
