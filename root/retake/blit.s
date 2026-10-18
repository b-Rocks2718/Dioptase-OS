# RETAKE hand-written copy loops (user mode, leaf routines).

  .text
  .align 4

# Copy a run of 32-bit words (two packed 0x0BGR pixels per word).
# Inputs:  r1 = destination, r2 = source (both word aligned),
#          r3 = number of words to copy (>= 0).
# Outputs: none. Clobbers r1-r11 (caller-saved temporaries only).
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
