  .text
  .align 4

  .global memcpy
memcpy:
  # Purpose: copy `n` bytes from `src` into `dest` for raw kernel buffers.
  # Inputs: r1 = dest, r2 = src, r3 = byte count.
  # Outputs: r1 = original dest pointer.
  # Preconditions: `src` and `dest` are valid for `n` bytes and do not overlap.
  # ABI notes: uses only caller-saved temporaries (docs/abi.md Registers).
  mov  r4, r1
memcpy_loop:
  cmp  r3, 0
  bz   memcpy_done
  lba  r5, [r2]
  sba  r5, [r1]
  add  r1, r1, 1
  add  r2, r2, 1
  add  r3, r3, -1
  jmp  memcpy_loop
memcpy_done:
  mov  r1, r4
  ret

  .global memcpy2
memcpy2:
  # Purpose: copy `n` bytes from `src` into `dest` using 2-byte load/store
  # operations for aligned PCM/MMIO paths.
  # Inputs: r1 = dest, r2 = src, r3 = byte count.
  # Outputs: r1 = original dest pointer.
  # Preconditions:
  # - `src` and `dest` are valid for `n` bytes and do not overlap
  # - `src` and `dest` are both 2-byte aligned
  # - `n` is even
  # ISA notes:
  # - `lda`/`sda` move one 16-bit "double" per iteration
  # - docs/ISA.md says misaligned loads/stores round down, so callers must meet
  #   the alignment preconditions instead of relying on implicit rounding
  # ABI notes: uses only caller-saved temporaries (docs/abi.md Registers).
  mov  r4, r1
memcpy2_loop:
  cmp  r3, 0
  bz   memcpy2_done
  lda  r5, [r2]
  sda  r5, [r1]
  add  r1, r1, 2
  add  r2, r2, 2
  add  r3, r3, -2
  jmp  memcpy2_loop
memcpy2_done:
  mov  r1, r4
  ret

  .global memcpy4
memcpy4:
  # Purpose: copy `n` bytes from `src` into `dest` using 4-byte load/store
  # operations for aligned hot paths.
  # Inputs: r1 = dest, r2 = src, r3 = byte count.
  # Outputs: r1 = original dest pointer.
  # Preconditions:
  # - `src` and `dest` are valid for `n` bytes and do not overlap
  # - `src` and `dest` are both 4-byte aligned
  # - `n` is a multiple of 4
  # ISA notes:
  # - `lwa`/`swa` move one 32-bit word per iteration
  # - docs/ISA.md says misaligned loads/stores round down, so callers must meet
  #   the alignment preconditions exactly
  # ABI notes: uses only caller-saved temporaries (docs/abi.md Registers).
  mov  r4, r1
memcpy4_loop:
  cmp  r3, 0
  bz   memcpy4_done
  lwa  r5, [r2]
  swa  r5, [r1]
  add  r1, r1, 4
  add  r2, r2, 4
  add  r3, r3, -4
  jmp  memcpy4_loop
memcpy4_done:
  mov  r1, r4
  ret

  .global memset
memset:
  # Purpose: fill `n` bytes at `dest` with the low byte of `c`.
  # Inputs: r1 = dest, r2 = fill value, r3 = byte count.
  # Outputs: r1 = original dest pointer.
  # ABI notes: uses only caller-saved temporaries (docs/abi.md Registers).
  mov  r4, r1
memset_loop:
  cmp  r3, 0
  bz   memset_done
  sba  r2, [r1]
  add  r1, r1, 1
  add  r3, r3, -1
  jmp  memset_loop
memset_done:
  mov  r1, r4
  ret
