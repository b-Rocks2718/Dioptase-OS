  # BIOS reset stub
  # Reset PC is at physical address 0x400.
  # Current BIOS startup assumes only the boot core executes this path before
  # the kernel wakes any secondary cores

.define BIOS_STACK_TOP, 0x10000

  # BIOS entry point
  .origin 0x400
  .text
  .global _start
_start:
  # Initialize the temporary BIOS stack. The stack grows downward from low
  # physical memory and is only used until bios_entry hands off to the kernel.
  movi sp, BIOS_STACK_TOP

  # Switch to the C BIOS once a stack is available.
  call bios_entry

  # bios_entry only returns after a fatal boot error.
  mode halt
