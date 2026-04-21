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
