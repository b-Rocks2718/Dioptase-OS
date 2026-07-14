# Kernel Memory Map

### 0x0000000 - 0x00003FF
Interrupt Vector Table

### 0x400 - ...
Where BIOS code is loaded (32KiB reserved). Can overwrite once kernel is entered.

### 0x10000 - 0x90000
Kernel text (512KiB reserved for now)

### 0x90000 - 0xD0000
Kernel data (256KiB)

### 0xD0000 - 0xE0000
Kernel rodata (64KiB)

### 0xE0000 - 0xF0000
Kernel bss (64KiB)

### 0xF0000 - 0x100000
Kernel Stacks (16KiB each)
- Core 3: 0xF0000 - 0xF4000
- Core 2: 0xF4000 - 0xF8000
- Core 1: 0xF8000 - 0xFC000
- Core 0: 0xFC000 - 0x100000

### 0x100000 - 0x7FB7FFF
Physical frames to allocate (~127MiB of frames, 0x7EB8 = 32440 frames total).
The kernel heap is backed by frames from this region; see `heap.md`.

### 0x7FB8000 - 0x7FFFFFF
I/O devices - see [memory map](https://github.com/b-Rocks2718/Dioptase/blob/main/docs/mem_map.md)
