# SKYFORGE user-mode assembly fast paths (docs/ISA.md, docs/abi.md).
#
# Full 320x192 single-pixel backbuffer layout:
# Two halves of 96 rows split at the horizon. Each half consists of 40 chunks
# of 8x96 pixels (one per 8-pixel column group), row-major with 16-byte rows,
# so pixel (x, y) is at byte offset:
#   (y >= 96) * 61440 + (x >> 3) * 1536 + (y % 96) * 16 + (x & 7) * 2.
# Stepping down a column is a 16-byte stride that jumps HALF_SKIP (59,904)
# bytes when crossing row 96, keeping each column walk in two 4 KiB TLB pages.

  .text
  .align 4

# Copy column-groups [g0, g1) of the 320x192 view from the backbuffer to the
# pixel framebuffer in 32-row bands so both buffers stay inside the 16-entry TLB.
# Inputs:  r1 = framebuffer base, r2 = backbuffer base, r3 = g0, r4 = g1 (0 <= g0 < g1 <= 40).
# Outputs: none. Clobbers r1-r16.
  .global blit_strip_groups
blit_strip_groups:
  sub  r4, r4, r3
  lsl  r6, r3, 4
  add  r1, r1, r6
  movi r5, 1536
  mov  r6, r0
blit_strip_g0:
  cmp  r3, r0
  bz   blit_strip_g0_done
  add  r6, r6, r5
  add  r3, r3, -1
  jmp  blit_strip_g0
blit_strip_g0_done:
  add  r2, r2, r6
  movi r12, 624
  movi r14, 20464
  movi r15, 1024
  movi r13, 6
  movi r16, 3
blit_strip_band:
  mov  r5, r4
  mov  r6, r1
  mov  r7, r2
blit_strip_chunk:
  movi r8, 32
blit_strip_row:
  lwa  r9, [r7], 4
  lwa  r10, [r7], 4
  lwa  r11, [r7], 4
  lwa  r3, [r7], 4
  swa  r9, [r6], 4
  swa  r10, [r6], 4
  swa  r11, [r6], 4
  swa  r3, [r6], 4
  add  r6, r6, r12
  add  r8, r8, -1
  bnz  blit_strip_row
  sub  r6, r6, r14
  add  r7, r7, r15
  add  r5, r5, -1
  bnz  blit_strip_chunk
  movi r5, 20480
  add  r1, r1, r5
  add  r2, r2, 512
  add  r16, r16, -1
  bnz  blit_strip_same_half
  movi r5, 59904
  add  r2, r2, r5
  movi r16, 3
blit_strip_same_half:
  add  r13, r13, -1
  bnz  blit_strip_band
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

# Draw one textured and shaded vertical terrain/cliff span down a view column.
# Inputs:  r1 = first destination pixel (short*), r2 = pixel count (> 0),
#          r3 = 64-byte texture column, r4 = texpos (16.16), r5 = texstep (16.16),
#          r6 = shade table (256 halfwords), r7 = rows left in top half (HALF_LEFT(y)).
# Outputs: none. Clobbers r1, r2, r4, r7-r9.
  .global draw_voxel_span
draw_voxel_span:
  movi r9, 59904
draw_voxel_loop:
  lsr  r8, r4, 16
  and  r8, r8, 63
  add  r8, r3, r8
  lba  r8, [r8]
  lsl  r8, r8, 1
  add  r8, r6, r8
  lda  r8, [r8]
  sda  r8, [r1], 16
  add  r4, r4, r5
  add  r7, r7, -1
  bz   draw_voxel_half
draw_voxel_next:
  add  r2, r2, -1
  bnz  draw_voxel_loop
  ret
draw_voxel_half:
  add  r1, r1, r9
  movi r7, 96
  jmp  draw_voxel_next

# Draw one column of a 64x64 billboard sprite; palette index 0 is transparent.
# Inputs:  r1 = first destination pixel, r2 = pixel count (> 0),
#          r3 = 64-byte sprite texel column, r4 = texpos (16.16),
#          r5 = texstep (16.16), r6 = shade table, r7 = HALF_LEFT(y).
# Outputs: none. Clobbers r1, r2, r4, r7-r9.
  .global draw_sprite_col
draw_sprite_col:
  movi r9, 59904
draw_sprite_col_loop:
  lsr  r8, r4, 16
  and  r8, r8, 63
  add  r8, r3, r8
  lba  r8, [r8]
  cmp  r8, r0
  bz   draw_sprite_col_skip
  lsl  r8, r8, 1
  add  r8, r6, r8
  lda  r8, [r8]
  sda  r8, [r1]
draw_sprite_col_skip:
  add  r1, r1, 16
  add  r4, r4, r5
  add  r7, r7, -1
  bz   draw_sprite_col_half
draw_sprite_col_next:
  add  r2, r2, -1
  bnz  draw_sprite_col_loop
  ret
draw_sprite_col_half:
  add  r1, r1, r9
  movi r7, 96
  jmp  draw_sprite_col_next

# Draw one column of a 12-bit foreground image (the player starfighter); pixel 0 is transparent.
# Inputs:  r1 = first destination pixel, r2 = pixel count (> 0),
#          r3 = first source pixel (column-major halfwords), r4 = HALF_LEFT(y).
# Outputs: none. Clobbers r1-r4, r8, r9.
  .global draw_image_col
draw_image_col:
  movi r9, 59904
draw_image_col_loop:
  lda  r8, [r3], 2
  cmp  r8, r0
  bz   draw_image_col_skip
  sda  r8, [r1]
draw_image_col_skip:
  add  r1, r1, 16
  add  r4, r4, -1
  bz   draw_image_col_half
draw_image_col_next:
  add  r2, r2, -1
  bnz  draw_image_col_loop
  ret
draw_image_col_half:
  add  r1, r1, r9
  movi r4, 96
  jmp  draw_image_col_next

# Unsigned 16 x 16 -> 32 bit multiply by quarter squares (qsq_center in assets.s).
# Inputs:  r1 = a (0..65535), r2 = b (0..65535).
# Outputs: r1 = a * b. Clobbers r1-r12.
  .global umul16
umul16:
  adpc r9, qsq_center
  and  r3, r1, 0xFF
  lsr  r4, r1, 8
  and  r5, r2, 0xFF
  lsr  r6, r2, 8
  add  r7, r3, r5
  sub  r8, r3, r5
  lsl  r7, r7, 1
  lsl  r8, r8, 1
  add  r7, r9, r7
  add  r8, r9, r8
  lda  r7, [r7]
  lda  r8, [r8]
  sub  r10, r7, r8
  add  r7, r4, r5
  sub  r8, r4, r5
  lsl  r7, r7, 1
  lsl  r8, r8, 1
  add  r7, r9, r7
  add  r8, r9, r8
  lda  r7, [r7]
  lda  r8, [r8]
  sub  r11, r7, r8
  add  r7, r3, r6
  sub  r8, r3, r6
  lsl  r7, r7, 1
  lsl  r8, r8, 1
  add  r7, r9, r7
  add  r8, r9, r8
  lda  r7, [r7]
  lda  r8, [r8]
  sub  r12, r7, r8
  add  r11, r11, r12
  add  r7, r4, r6
  sub  r8, r4, r6
  lsl  r7, r7, 1
  lsl  r8, r8, 1
  add  r7, r9, r7
  add  r8, r9, r8
  lda  r7, [r7]
  lda  r8, [r8]
  sub  r12, r7, r8
  lsl  r12, r12, 16
  lsl  r11, r11, 8
  add  r1, r12, r11
  add  r1, r1, r10
  ret

# Unsigned 32 / 32 division skipping leading zero bytes.
# Inputs:  r1 = dividend n, r2 = divisor d (nonzero).
# Outputs: r1 = n / d. Clobbers r1-r6.
  .global udiv32
udiv32:
  cmp  r2, r0
  bz   udiv32_zero
  mov  r3, r0
  mov  r4, r0
  movi r5, 32
udiv32_skip:
  lsr  r6, r1, 24
  bnz  udiv32_loop
  lsl  r1, r1, 8
  add  r5, r5, -8
  bnz  udiv32_skip
  mov  r1, r0
  ret
udiv32_loop:
  lsl  r1, r1, 1
  addc r4, r4, r4
  lsl  r3, r3, 1
  cmp  r4, r2
  bb   udiv32_next
  sub  r4, r4, r2
  add  r3, r3, 1
udiv32_next:
  add  r5, r5, -1
  bnz  udiv32_loop
  mov  r1, r3
  ret
udiv32_zero:
  mov  r1, r0
  ret

# Add one wavetable SFX voice into the audio mix accumulator.
# Inputs:  r1 = int accumulator, r2 = sample count (> 0),
#          r3 = wavetable (64 signed halfwords), r4 = phase, r5 = phase step.
# Outputs: none. Clobbers r1, r2, r4, r6-r8.
  .global mix_add
mix_add:
  lsr  r6, r4, 26
  lsl  r6, r6, 1
  add  r6, r3, r6
  lda  r7, [r6]
  sxtd r7, r7
  lwa  r8, [r1]
  add  r8, r8, r7
  swa  r8, [r1], 4
  add  r4, r4, r5
  add  r2, r2, -1
  bnz  mix_add
  ret

# Add one mu-law sampled music voice into the audio mix accumulator.
# Inputs:  r1 = int accumulator, r2 = sample count (> 0),
#          r3 = mu-law sample bytes, r4 = pos (16.16), r5 = step (16.16),
#          r6 = end (length << 16), r7 = loop_len << 16 (0 = one-shot),
#          r8 = volume row (256 signed halfwords).
# Outputs: r1 = updated position (or r6 if one-shot ended). Clobbers r2, r4, r9, r10.
  .global mix_sample
mix_sample:
mix_sample_loop:
  lsr  r9, r4, 16
  add  r9, r3, r9
  lba  r9, [r9]
  lsl  r9, r9, 1
  add  r9, r8, r9
  lda  r9, [r9]
  sxtd r9, r9
  lwa  r10, [r1]
  add  r10, r10, r9
  swa  r10, [r1], 4
  add  r4, r4, r5
  cmp  r4, r6
  bae  mix_sample_wrap
mix_sample_next:
  add  r2, r2, -1
  bnz  mix_sample_loop
  mov  r1, r4
  ret
mix_sample_wrap:
  cmp  r7, r0
  bz   mix_sample_done
  sub  r4, r4, r7
  cmp  r4, r6
  bae  mix_sample_wrap
  jmp  mix_sample_next
mix_sample_done:
  mov  r1, r6
  ret

# Apply two damped feedback comb filters and an allpass diffuser to the audio mix.
# Inputs:  r1 = int mix, r2 = short output, r3 = sample count (> 0), r4 = struct Reverb*.
# Outputs: none. Clobbers r1-r3, r5-r19.
  .global reverb_mix
reverb_mix:
  lwa  r5, [r4, 8]
  lwa  r6, [r4, 4]
  lwa  r7, [r4, 20]
  lwa  r8, [r4, 16]
  lwa  r9, [r4, 32]
  lwa  r10, [r4, 28]
  lwa  r11, [r4, 36]
  lwa  r12, [r4, 40]
  movi r18, 32767
  movi r19, -32768
reverb_loop:
  lwa  r13, [r1], 4
  lwa  r14, [r5]
  sub  r16, r14, r11
  asr  r16, r16, 2
  add  r11, r11, r16
  asr  r16, r13, 1
  asr  r17, r11, 1
  add  r16, r16, r17
  asr  r17, r11, 4
  add  r16, r16, r17
  swa  r16, [r5], 4
  cmp  r5, r6
  bnz  reverb_comb1_ok
  lwa  r5, [r4, 0]
reverb_comb1_ok:
  lwa  r15, [r7]
  sub  r16, r15, r12
  asr  r16, r16, 2
  add  r12, r12, r16
  asr  r16, r13, 1
  asr  r17, r12, 1
  add  r16, r16, r17
  asr  r17, r12, 4
  add  r16, r16, r17
  swa  r16, [r7], 4
  cmp  r7, r8
  bnz  reverb_comb2_ok
  lwa  r7, [r4, 12]
reverb_comb2_ok:
  add  r14, r14, r15
  asr  r14, r14, 1
  lwa  r15, [r9]
  asr  r16, r14, 1
  sub  r15, r15, r16
  asr  r16, r15, 1
  add  r16, r14, r16
  swa  r16, [r9], 4
  cmp  r9, r10
  bnz  reverb_allpass_ok
  lwa  r9, [r4, 24]
reverb_allpass_ok:
  asr  r15, r15, 2
  add  r13, r13, r15
  cmp  r13, r18
  bg   reverb_high
  cmp  r13, r19
  bl   reverb_low
reverb_store:
  sda  r13, [r2], 2
  add  r3, r3, -1
  bnz  reverb_loop
  swa  r5, [r4, 8]
  swa  r7, [r4, 20]
  swa  r9, [r4, 32]
  swa  r11, [r4, 36]
  swa  r12, [r4, 40]
  ret
reverb_high:
  mov  r13, r18
  jmp  reverb_store
reverb_low:
  mov  r13, r19
  jmp  reverb_store
