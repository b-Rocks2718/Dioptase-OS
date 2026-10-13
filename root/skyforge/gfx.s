# SKYFORGE user-mode assembly fast paths (docs/ISA.md, docs/abi.md).
#
# All routines are user-mode leaf functions adhering to the Dioptase ABI:
# arguments arrive in r1-r8, return value in r1, caller-saved registers r1-r19
# may be clobbered, and callee-saved registers r20-r27 are preserved.

  .text
  .align 4

# Copy column-groups [g0, g1) from the chunked 3D backbuffer to the 320x192
# pixel framebuffer in 32-row bands so both buffers stay inside the 16-entry TLB.
# Each group is 8 double-columns (32 bytes per row) x 192 rows (6,144 bytes).
# Inputs:  r1 = pixel framebuffer base (from get_pixel_fb),
#          r2 = backbuffer base (g_view_words, 4-byte aligned),
#          r3 = g0, r4 = g1 (0 <= g0 < g1 <= 20).
# Outputs: none. Clobbers r1-r19.
  .global blit_strip_groups
blit_strip_groups:
  sub  r4, r4, r3              # number of groups in [g0, g1)
  lsl  r5, r3, 5               # g0 * 32: byte offset of group g0 in a framebuffer row
  add  r1, r1, r5
  lsl  r5, r3, 12              # g0 * 4096
  lsl  r6, r3, 11              # g0 * 2048
  add  r5, r5, r6              # g0 * 6144: byte offset of group g0 in backbuffer
  add  r2, r2, r5
  movi r14, 20448              # 32 * 640 - 32: step from group bottom to next group top
  movi r15, 5120               # 6144 - 1024: step to same band of next group
  movi r16, 20480              # 32 * 640: one 32-row band in framebuffer
  movi r13, 6                  # 6 bands of 32 rows = 192 rows
blit_band_loop:
  mov  r5, r4                  # groups remaining in this band
  mov  r6, r1                  # framebuffer pointer for group g0 in this band
  mov  r7, r2                  # backbuffer pointer for group g0 in this band
blit_group_loop:
  movi r8, 32                  # 32 rows per band
blit_row_loop:
  lwa  r9,  [r7], 4
  lwa  r10, [r7], 4
  lwa  r11, [r7], 4
  lwa  r12, [r7], 4
  lwa  r17, [r7], 4
  lwa  r18, [r7], 4
  lwa  r19, [r7], 4
  lwa  r3,  [r7], 4
  swa  r9,  [r6], 4
  swa  r10, [r6], 4
  swa  r11, [r6], 4
  swa  r12, [r6], 4
  swa  r17, [r6], 4
  swa  r18, [r6], 4
  swa  r19, [r6], 4
  swa  r3,  [r6], 4
  add  r6, r6, 608             # advance to next framebuffer row (640 - 32)
  add  r8, r8, -1
  bnz  blit_row_loop
  sub  r6, r6, r14             # advance framebuffer pointer to top of next group
  add  r7, r7, r15             # advance backbuffer pointer to same band of next group
  add  r5, r5, -1
  bnz  blit_group_loop
  add  r1, r1, r16             # advance framebuffer base by 32 rows
  add  r2, r2, 1024            # advance backbuffer base by 32 group rows (1024 bytes)
  add  r13, r13, -1
  bnz  blit_band_loop
  ret

# Copy `words` 32-bit words from `src` to `dst`.
# Inputs:  r1 = dst, r2 = src, r3 = words (> 0).
# Outputs: none. Clobbers r1-r4.
  .global copy_words
copy_words:
  cmp  r3, r0
  bz   copy_words_done
copy_words_loop:
  lwa  r4, [r2], 4
  swa  r4, [r1], 4
  add  r3, r3, -1
  bnz  copy_words_loop
copy_words_done:
  ret

# Fill `words` 32-bit words at `dst` with `value`.
# Inputs:  r1 = dst, r2 = value, r3 = words (> 0).
# Outputs: none. Clobbers r1, r3.
  .global fill_words
fill_words:
  cmp  r3, r0
  bz   fill_words_done
fill_words_loop:
  swa  r2, [r1], 4
  add  r3, r3, -1
  bnz  fill_words_loop
fill_words_done:
  ret

# Multiply two 16-bit unsigned integers using shift-and-add with early exit.
# Inputs:  r1 = a (0..65535), r2 = b (0..65535).
# Outputs: r1 = a * b. Clobbers r1-r4.
  .global umul16
umul16:
  mov  r3, r1
  mov  r1, r0
  cmp  r2, r0
  bz   umul16_done
umul16_loop:
  and  r4, r2, 1
  bz   umul16_skip
  add  r1, r1, r3
umul16_skip:
  lsl  r3, r3, 1
  lsr  r2, r2, 1
  bnz  umul16_loop
umul16_done:
  ret

# Divide 32-bit unsigned integer `n` by `d` using binary restoring division.
# Inputs:  r1 = numerator n, r2 = denominator d (> 0).
# Outputs: r1 = n / d. Clobbers r1-r6.
  .global udiv32
udiv32:
  cmp  r2, r0
  bz   udiv32_zero
  mov  r3, r1                  # numerator shift register
  mov  r1, r0                  # quotient
  mov  r4, r0                  # remainder
  movi r5, 32
udiv32_loop:
  lsl  r4, r4, 1
  lsr  r6, r3, 31
  or   r4, r4, r6
  lsl  r3, r3, 1
  lsl  r1, r1, 1
  cmp  r4, r2
  bb   udiv32_next
  sub  r4, r4, r2
  or   r1, r1, 1
udiv32_next:
  add  r5, r5, -1
  bnz  udiv32_loop
  ret
udiv32_zero:
  mov  r1, r0
  ret
