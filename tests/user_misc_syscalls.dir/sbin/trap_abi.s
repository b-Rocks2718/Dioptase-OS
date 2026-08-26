  .text
  .align 4

  # Return 1 only if a raw trap preserves the user r29 required by the trap
  # ABI. Unlike the CRT syscall wrappers, this helper deliberately does not
  # hide a kernel clobber by restoring r29 immediately after the trap.
  .global raw_trap_preserves_ra
raw_trap_preserves_ra:
  push ra
  push r20

  movi r20, 1234
  mov  ra, r20

  # TRAP_GET_CURRENT_JIFFIES is code 2 and has no output or arguments. Its
  # return value is irrelevant here; the sentinel comparison observes only the
  # trap-callee-saved r29 register.
  movi r1, 2
  trap

  cmp  ra, r20
  bz   raw_trap_ra_preserved
  mov  r1, r0
  jmp  raw_trap_ra_done

raw_trap_ra_preserved:
  movi r1, 1

raw_trap_ra_done:
  pop  r20
  pop  ra
  ret
