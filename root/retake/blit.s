# RETAKE hand-written pixel loops (user mode, leaf routines).
#
# Pixels are 16-bit 0x0BGR values, two per 32-bit word (little-endian: the
# left pixel is the low half). 0xF000 marks a transparent pixel; opaque
# pixels never set bits 15:12. Every routine here uses only caller-saved
# registers (r1-r19, docs/abi.md) and needs word-aligned pointers.

  .text
  .align 4

# Copy a run of 32-bit words.
# Inputs:  r1 = destination, r2 = source, r3 = number of words (>= 0).
# Outputs: none. Clobbers r1-r11.
  .global tl_copy_words
tl_copy_words:
  add  r3, r3, -8
  bl   tl_copy_words_tail       # fewer than 8 words in total
tl_copy_words_loop8:
  lwa  r4, [r2], 4
  lwa  r5, [r2], 4
  lwa  r6, [r2], 4
  lwa  r7, [r2], 4
  lwa  r8, [r2], 4
  lwa  r9, [r2], 4
  lwa  r10, [r2], 4
  lwa  r11, [r2], 4
  swa  r4, [r1], 4
  swa  r5, [r1], 4
  swa  r6, [r1], 4
  swa  r7, [r1], 4
  swa  r8, [r1], 4
  swa  r9, [r1], 4
  swa  r10, [r1], 4
  swa  r11, [r1], 4
  add  r3, r3, -8
  bge  tl_copy_words_loop8
tl_copy_words_tail:
  add  r3, r3, 8                # 0..7 words remain
  bz   tl_copy_words_done
tl_copy_words_loop1:
  lwa  r4, [r2], 4
  swa  r4, [r1], 4
  add  r3, r3, -1
  bnz  tl_copy_words_loop1
tl_copy_words_done:
  ret

# Composite one row: room pixels over backdrop pixels.
# For each word, opaque room pixels win and transparent ones (0xF000) take
# the backdrop pixel underneath. Fully opaque words, the common case, cost a
# load pair, a test and a store.
# Inputs:  r1 = destination row, r2 = room row, r3 = backdrop row,
#          r4 = number of words (> 0).
# Outputs: none. Clobbers r1-r12.
  .global rt_composite
rt_composite:
  movi r9, 0xF000F000           # transparency nibbles of both pixels
  movi r10, 0xF000              # transparency nibble of the left pixel
  movi r11, 0xFFFF0000          # right pixel
  movi r12, 0xFFFF              # left pixel
rt_composite_loop:
  lwa  r5, [r2], 4
  lwa  r6, [r3], 4
  and  r7, r5, r9
  bnz  rt_composite_mixed
  swa  r5, [r1], 4
  add  r4, r4, -1
  bnz  rt_composite_loop
  ret
rt_composite_mixed:
  sub  r8, r7, r9
  bnz  rt_composite_half        # exactly one pixel is transparent
  swa  r6, [r1], 4              # both transparent: all backdrop
  add  r4, r4, -1
  bnz  rt_composite_loop
  ret
rt_composite_half:
  and  r8, r7, r10
  bz   rt_composite_right       # left pixel opaque, so the right one is clear
  and  r5, r5, r11              # left clear: room's right + backdrop's left
  and  r6, r6, r12
  or   r5, r5, r6
  swa  r5, [r1], 4
  add  r4, r4, -1
  bnz  rt_composite_loop
  ret
rt_composite_right:
  and  r5, r5, r12              # right clear: room's left + backdrop's right
  and  r6, r6, r11
  or   r5, r5, r6
  swa  r5, [r1], 4
  add  r4, r4, -1
  bnz  rt_composite_loop
  ret
