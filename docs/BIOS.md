# Dioptase BIOS

The BIOS expects sd slot zero has a bootable drive. A drive is marked bootable by having the last two bytes of the first 512 byte sector set to 0xAA55.

If the BIOS finds a bootable drive, it reads the first 12 bytes to figure out how to load the kernel.  
The first 4 bytes of the boot sector specify a block offset for where in the drive the kernel code begins
The next 4 bytes specify the number of blocks the kernel code takes up.
The final 4 bytes specify the physical address to load the kernel at.

The BIOS will use this info to load the kernel and then jump to the address it loaded it at.
