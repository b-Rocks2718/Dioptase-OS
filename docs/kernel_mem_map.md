# Kernel Memory Map

### 0x0000000 - 0x00003FF
Interrupt Vector Table

### 0x400 - 0x8400
BIOS (32KiB reserved). Can overwrite once kernel is entered.
- BIOS image (text/data) is loaded at 0x400 (the reset PC)
- 0x5000 - 0x5200: temporary buffer for SD block 0 (MBR), see `MBR_LOAD_ADDRESS` in `bios/bios_entry.c`

### 0x8400 - 0x10000
BIOS stack (~31KiB). Grows down from 0x10000 (`BIOS_STACK_TOP` in `bios/init.s`).
Only the boot core uses it, and only until the BIOS jumps to the kernel. Can overwrite once kernel is entered.

### 0x10000 - 0xB0000
Kernel text (640KiB reserved for now)

### 0xB0000 - 0xE0000
Kernel data (192KiB)

### 0xE0000 - 0xE8000
Kernel rodata (32KiB)

### 0xE8000 - 0xF0000
Kernel bss (32KiB)

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
