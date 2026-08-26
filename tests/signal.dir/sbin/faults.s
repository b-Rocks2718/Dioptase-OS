  .text
  .align 4

  # Opcode 23 is architecturally reserved and must raise the invalid
  # instruction exception. The handler is expected to exit, so the following
  # return is reachable only if exception delivery is broken.
  .global trigger_invalid_instruction
trigger_invalid_instruction:
  .fill 0xB8000000
  ret

  # rfe is architecturally valid only in kernel mode. Executing it in this
  # user test must raise the privileged-instruction exception, which the OS
  # exposes through SIGNAL_ILL.
  .global trigger_privileged_instruction
trigger_privileged_instruction:
  # The user-mode assembler intentionally rejects privileged mnemonics, so
  # emit the ISA's canonical rfe encoding directly for this negative test.
  .fill 0xF8003000
  ret

  # Absolute branch to a deliberately halfword-aligned, but not word-aligned,
  # instruction address. Dioptase instruction PCs must be 4-byte aligned.
  .global trigger_misaligned_pc
trigger_misaligned_pc:
  adpc r1, aligned_pc_target
  add  r1, r1, 2
  bra  r0, r1

aligned_pc_target:
  ret
