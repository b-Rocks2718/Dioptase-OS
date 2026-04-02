# Dioptase BIOS

The BIOS expects sd slot zero has a bootable drive. A drive is marked bootable by having the last two bytes of the first 512 byte sector set to 0xAA55.

If the BIOS finds a bootable drive, it reads the first 44 bytes to figure out how to load the kernel. These bytes form an array of 11 4-byte integers. The entries determine how to load each section of the kernel into memory. Each section has a `start_block` for it's location within the drive, a `num_blocks` the section takes up within the drive, and a `load_address` for which physical address to write the section at in RAM. The exception is the bss section, which does not have a `start_block` field.  

```
[0..2]  text   = start_block, num_blocks, load_address
[3..5]  data   = start_block, num_blocks, load_address
[6..8]  rodata = start_block, num_blocks, load_address
[9..10] bss    = num_blocks, load_address
```

After loading each section, the bios jumps to the `load_address` of the text section.
