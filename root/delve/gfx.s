# DELVE hand-written inner loops (user mode).
#
# These are the per-pixel and per-column routines that dominate a frame. They
# are plain leaf functions under the Dioptase ABI (docs/abi.md): arguments in
# r1-r8, result in r1, and they only touch caller-saved r1-r19 unless noted
# (draw_flats_asm saves the callee-saved registers it uses on the stack).
# None of them trap or touch privileged state.
#
# Back-buffer layout (see delve.h, "Screen layout"): the 320x192 view is kept
# in two halves of 96 rows, split at the horizon. A half is 40 chunks of 8x96
# pixels (one per 8-pixel column group), and each chunk is row-major with
# 16-byte rows, so pixel (x, y) lives at byte offset
#   (y >= 96) * 61440 + (x >> 3) * 1536 + (y % 96) * 16 + (x & 7) * 2.
# Walking down a column is a 16-byte stride that jumps HALF_SKIP bytes when
# it crosses the horizon, so a column touches one chunk (about one page) per
# half; a chunk row is one 16-byte copy to the framebuffer.
#
# HALF_SKIP = 61440 - 1536 = 59904: the extra step from a chunk's last row to
# the same column group's first row in the bottom half.

  .text
  .align 4

# Copy column groups [g0, g1) of the finished view from the back buffer to
# the pixel framebuffer (row-major, 640-byte rows; docs/mem_map.md), 32 rows
# at a time, so the working set is the strip's slice of one half (a page per
# few groups) and five framebuffer pages.
# Inputs:  r1 = framebuffer pixel (0, 0) (the mapping from get_pixel_fb),
#          r2 = back buffer, r3 = g0, r4 = g1 (0 <= g0 < g1 <= 40).
#          Both buffers word aligned.
# Outputs: none. Clobbers r1-r16.
  .global blit_strip
blit_strip:
  sub  r4, r4, r3              # column groups in the strip
  lsl  r6, r3, 4               # g0 * 16: the strip's first framebuffer byte in a row
  add  r1, r1, r6
  movi r5, 1536
  mov  r6, r0
blit_strip_g0:                 # r6 = g0 * 1536 (no multiply instruction)
  cmp  r3, r0
  bz   blit_strip_g0_done
  add  r6, r6, r5
  add  r3, r3, -1
  jmp  blit_strip_g0
blit_strip_g0_done:
  add  r2, r2, r6              # the strip's first chunk in the top half
  movi r12, 624                # framebuffer row stride minus the 16 bytes stored
  movi r14, 20464              # 32 rows down minus one chunk right: 32 * 640 - 16
  movi r15, 1024               # from a chunk slice's end to the next chunk's slice
  movi r13, 6                  # 32-row bands left
  movi r16, 3                  # bands left in this half
blit_strip_band:
  mov  r5, r4                  # chunks left in this band
  mov  r6, r1                  # framebuffer: band's top row, strip's first chunk
  mov  r7, r2                  # back buffer: this band's slice of the first chunk
blit_strip_chunk:
  movi r8, 32                  # rows left in the slice
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
  sub  r6, r6, r14             # the next chunk's top row in this band
  add  r7, r7, r15             # the same slice of the next chunk
  add  r5, r5, -1
  bnz  blit_strip_chunk
  # next band: 32 framebuffer rows on; in the back buffer the next 32-row
  # slice of each chunk, or after three bands the bottom half's first slice
  movi r5, 20480
  add  r1, r1, r5
  add  r2, r2, 512
  add  r16, r16, -1
  bnz  blit_strip_same_half
  movi r5, 59904               # HALF_SKIP: rows 96.. of the same chunks
  add  r2, r2, r5
  movi r16, 3
blit_strip_same_half:
  add  r13, r13, -1
  bnz  blit_strip_band
  ret

# Copy the column groups g in [g0, g1) with own[g] != 0 from the back buffer
# to the framebuffer. The copy is blit_strip's, still 32 rows at a time
# across all the groups, so a band's five framebuffer pages stay in the TLB
# while every owned group's slice of the band is copied; copying group by
# group instead would walk all 30 framebuffer pages once per group.
# Inputs:  r1 = framebuffer pixel (0, 0), r2 = back buffer,
#          r3 = own: one byte per column group (nonzero = copy it),
#          r4 = g0, r5 = g1 (0 <= g0 < g1 <= 40).
# Outputs: none. Clobbers r1-r19.
  .global blit_groups
blit_groups:
  sub  r5, r5, r4              # groups in the range
  add  r3, r3, r4              # &own[g0]
  lsl  r6, r4, 4               # g0 * 16: the range's first framebuffer byte in a row
  add  r1, r1, r6
  movi r7, 1536
  mov  r6, r0
blit_groups_g0:                # r6 = g0 * 1536 (no multiply instruction)
  cmp  r4, r0
  bz   blit_groups_g0_done
  add  r6, r6, r7
  add  r4, r4, -1
  jmp  blit_groups_g0
blit_groups_g0_done:
  add  r2, r2, r6              # group g0's chunk in the top half
  movi r12, 624                # framebuffer row stride minus the 16 bytes stored
  movi r14, 20464              # 32 rows down minus one chunk right: 32 * 640 - 16
  movi r15, 1024               # from a chunk slice's end to the next chunk's slice
  movi r13, 6                  # 32-row bands left
  movi r16, 3                  # bands left in this half
blit_groups_band:
  mov  r8, r5                  # groups left in this band
  mov  r6, r1                  # framebuffer: band's top row, group g0
  mov  r7, r2                  # back buffer: this band's slice of group g0
  mov  r9, r3                  # own cursor
blit_groups_group:
  lba  r10, [r9], 1
  cmp  r10, r0
  bz   blit_groups_skip
  movi r19, 32                 # rows left in the slice
blit_groups_row:
  lwa  r10, [r7], 4
  lwa  r11, [r7], 4
  lwa  r17, [r7], 4
  lwa  r18, [r7], 4
  swa  r10, [r6], 4
  swa  r11, [r6], 4
  swa  r17, [r6], 4
  swa  r18, [r6], 4
  add  r6, r6, r12
  add  r19, r19, -1
  bnz  blit_groups_row
  sub  r6, r6, r14             # the next group's top row in this band
  add  r7, r7, r15             # the same slice of the next group
  jmp  blit_groups_next
blit_groups_skip:
  add  r6, r6, 16              # the next group's columns
  add  r7, r7, 1536            # the same slice of the next group
blit_groups_next:
  add  r8, r8, -1
  bnz  blit_groups_group
  # next band: 32 framebuffer rows on; in the back buffer the next 32-row
  # slice of each chunk, or after three bands the bottom half's first slice
  movi r6, 20480
  add  r1, r1, r6
  add  r2, r2, 512
  add  r16, r16, -1
  bnz  blit_groups_same_half
  movi r6, 59904               # HALF_SKIP: rows 96.. of the same chunks
  add  r2, r2, r6
  movi r16, 3
blit_groups_same_half:
  add  r13, r13, -1
  bnz  blit_groups_band
  ret

# Copy `words` 32-bit words (generic memcpy for word-aligned buffers).
# Inputs:  r1 = destination, r2 = source, r3 = word count (> 0).
# Outputs: none. Clobbers r1-r4.
  .global copy_words
copy_words:
  lwa  r4, [r2], 4
  swa  r4, [r1], 4
  add  r3, r3, -1
  bnz  copy_words
  ret

# Draw one textured wall slice down a view column.
# Inputs:  r1 = first destination pixel (back buffer), r2 = pixel count (> 0),
#          r3 = texture column (64 palette-index bytes),
#          r4 = texture position (16.16 texels, < 64.0 for every pixel drawn),
#          r5 = texture step per pixel (16.16), r6 = shade table (256 halfwords),
#          r7 = rows before the column crosses into the bottom half
#               (HALF_LEFT(y) in delve.h).
# Outputs: none. Clobbers r1, r2, r4, r7-r9.
  .global draw_wall
draw_wall:
  movi r9, 59904               # HALF_SKIP
draw_wall_loop:
  lsr  r8, r4, 16
  add  r8, r3, r8
  lba  r8, [r8]
  lsl  r8, r8, 1
  add  r8, r6, r8
  lda  r8, [r8]
  sda  r8, [r1], 16
  add  r4, r4, r5
  add  r7, r7, -1
  bz   draw_wall_half
draw_wall_next:
  add  r2, r2, -1
  bnz  draw_wall_loop
  ret
draw_wall_half:
  add  r1, r1, r9              # into the same column group of the bottom half
  movi r7, 96
  jmp  draw_wall_next

# Unsigned 16 x 16 -> 32 bit multiply by quarter squares:
#   a * b = Q(a + b) - Q(a - b), Q(n) = floor(n * n / 4)
# applied to the four byte-by-byte partial products. `qsq_center` points at
# Q(0) in a 1021-entry halfword table covering -510..510 (assets.s).
# Inputs:  r1 = a, r2 = b, both < 65536.
# Outputs: r1 = a * b. Clobbers r1-r12.
  .global umul16
umul16:
  adpc r9, qsq_center
  and  r3, r1, 0xFF            # al
  lsr  r4, r1, 8               # ah
  and  r5, r2, 0xFF            # bl
  lsr  r6, r2, 8               # bh
  # p0 = al * bl
  add  r7, r3, r5
  sub  r8, r3, r5
  lsl  r7, r7, 1
  lsl  r8, r8, 1
  add  r7, r9, r7
  add  r8, r9, r8
  lda  r7, [r7]
  lda  r8, [r8]
  sub  r10, r7, r8
  # p1 = ah * bl
  add  r7, r4, r5
  sub  r8, r4, r5
  lsl  r7, r7, 1
  lsl  r8, r8, 1
  add  r7, r9, r7
  add  r8, r9, r8
  lda  r7, [r7]
  lda  r8, [r8]
  sub  r11, r7, r8
  # p2 = al * bh
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
  # p3 = ah * bh
  add  r7, r4, r6
  sub  r8, r4, r6
  lsl  r7, r7, 1
  lsl  r8, r8, 1
  add  r7, r9, r7
  add  r8, r9, r8
  lda  r7, [r7]
  lda  r8, [r8]
  sub  r12, r7, r8
  # combine: p3 << 16 + (p1 + p2) << 8 + p0
  lsl  r12, r12, 16
  lsl  r11, r11, 8
  add  r1, r12, r11
  add  r1, r1, r10
  ret

# Unsigned 32 / 32 division (restoring, one quotient bit per step). Leading
# zero bytes of the dividend are skipped first, so small dividends are cheap.
# Inputs:  r1 = dividend n, r2 = divisor d (nonzero).
# Outputs: r1 = n / d. Clobbers r1-r6.
  .global udiv32
udiv32:
  mov  r3, r0                  # quotient
  mov  r4, r0                  # remainder
  movi r5, 32                  # dividend bits left to consume
udiv32_skip:
  lsr  r6, r1, 24
  bnz  udiv32_loop
  lsl  r1, r1, 8
  add  r5, r5, -8
  bnz  udiv32_skip
  mov  r1, r0
  ret
udiv32_loop:
  lsl  r1, r1, 1               # carry = next dividend bit
  addc r4, r4, r4              # remainder = remainder * 2 + bit
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

# Cast the floor and ceiling over a strip of the view (row-based floor
# casting: along a screen row the texture coordinates advance linearly), lit
# by the frame's point lights.
#
# Pixels are visited one 8-pixel column group at a time (all 96 row pairs of
# one group before the next), so the back-buffer working set is one chunk in
# each half; walking whole screen rows instead would touch 40 chunks per row
# and thrash the 16-entry TLB.
#
# Row pair k is floor row 96 + k (row k of the group's bottom chunk) and
# ceiling row 95 - k (row 95 - k of its top chunk), so neither cursor ever
# leaves its chunk.
#
# Row pair k of group g is skipped when k < skip[g]: the walls drawn
# afterwards cover it in all eight columns. Skipped pairs still advance u and
# v so later groups stay right.
#
# Lighting: each 8-pixel run takes one light level, the row's distance level
# plus, for every light, max(0, (radius - d) >> 15) where d is the octagonal
# distance max + min / 2 from the run's center (one level per half cell,
# matching light_bonus() in render.c).
#
# Inputs:  r1 = 96 FlatRow records (delve.h), one per row pair k, holding the
#               row's current (u, v), per-pixel (du, dv), and base light
#               level. u and v are advanced in place across the groups.
#          r2 = the strip's first column group in the top half
#               (back buffer + g0 * 1536).
#          r3 = floor texture, r4 = ceiling texture (64x64, row-major).
#          r5 = FlatLight records (x, y, radius words), r6 = their count.
#          r7 = skip thresholds, one word per column group drawn.
#          r8 = number of column groups to draw (> 0).
# Outputs: none. Clobbers r1-r19; preserves r20-r27 (saved on the stack).
#
# Register map: r1 rows, r2 group base, r3/r4 textures, r5 lights, r6 light
# count, r7 skip cursor, r8 row cursor, r9-r12 u v du dv, r13/r14 floor and
# ceiling shade tables, r15/r16 floor and ceiling cursors, r17-r19 and
# r20-r23 scratch, r24 texel mask, r25 group counter, r26 shade_tables,
# r27 row pair index k.
  .global draw_flats_asm
draw_flats_asm:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  movi r24, 0xFC0              # mask for (v >> 4): texel row * 64
  mov  r25, r8                 # column groups to draw
  adpc r26, shade_tables
flats_group:
  mov  r8, r1                  # FlatRow cursor
  mov  r27, r0                 # k = 0
  movi r15, 61440              # floor row 96: bottom half, row 0
  add  r15, r2, r15
  movi r16, 1520               # ceiling row 95: top half, row 95 (95 * 16)
  add  r16, r2, r16
flats_row:
  lwa  r17, [r7]
  cmp  r27, r17
  bge  flats_visible
  # hidden behind walls: just advance this row's u and v by 8 pixels
  lwa  r9, [r8, 0]
  lwa  r11, [r8, 8]
  lsl  r11, r11, 3
  add  r9, r9, r11
  swa  r9, [r8, 0]
  lwa  r10, [r8, 4]
  lwa  r12, [r8, 12]
  lsl  r12, r12, 3
  add  r10, r10, r12
  swa  r10, [r8, 4]
  add  r15, r15, 16
  add  r16, r16, -16
  jmp  flats_row_done
flats_visible:
  lwa  r9, [r8, 0]             # u
  lwa  r10, [r8, 4]            # v
  lwa  r11, [r8, 8]            # du
  lwa  r12, [r8, 12]           # dv
  lwa  r13, [r8, 16]           # base light level
  # light the run at its center (u + 4 du, v + 4 dv)
  mov  r20, r6
  cmp  r20, r0
  bz   flats_lit
  lsl  r21, r11, 2
  add  r21, r9, r21            # center u
  lsl  r22, r12, 2
  add  r22, r10, r22           # center v
  mov  r23, r5
flats_light:
  lwa  r17, [r23, 0]
  sub  r17, r21, r17           # dx
  bns  flats_dx_pos
  sub  r17, r0, r17
flats_dx_pos:
  lwa  r18, [r23, 4]
  sub  r18, r22, r18           # dy
  bns  flats_dy_pos
  sub  r18, r0, r18
flats_dy_pos:
  cmp  r17, r18
  bge  flats_ordered
  mov  r19, r17
  mov  r17, r18
  mov  r18, r19
flats_ordered:
  lsr  r18, r18, 1
  add  r17, r17, r18           # d = max + min / 2
  lwa  r18, [r23, 8]
  sub  r18, r18, r17           # radius - d
  bs   flats_light_next
  lsr  r18, r18, 15
  add  r13, r13, r18
flats_light_next:
  add  r23, r23, 12
  add  r20, r20, -1
  bnz  flats_light
flats_lit:
  # clamp to 0..15 and select the shade table (512 bytes each)
  cmp  r13, r0
  bge  flats_level_low_ok
  mov  r13, r0
flats_level_low_ok:
  movi r17, 15
  cmp  r13, r17
  ble  flats_level_high_ok
  mov  r13, r17
flats_level_high_ok:
  lsl  r13, r13, 9
  add  r13, r26, r13
  mov  r14, r13
  movi r17, 8
flats_pixel:
  lsr  r18, r9, 10
  and  r18, r18, 63
  lsr  r19, r10, 4
  and  r19, r19, r24
  or   r18, r18, r19           # texel index (v_texel * 64 + u_texel)
  add  r19, r3, r18
  lba  r19, [r19]
  lsl  r19, r19, 1
  add  r19, r13, r19
  lda  r19, [r19]
  sda  r19, [r15], 2
  add  r20, r4, r18
  lba  r20, [r20]
  lsl  r20, r20, 1
  add  r20, r14, r20
  lda  r20, [r20]
  sda  r20, [r16], 2
  add  r9, r9, r11
  add  r10, r10, r12
  add  r17, r17, -1
  bnz  flats_pixel
  swa  r9, [r8, 0]
  swa  r10, [r8, 4]
  # The floor cursor already sits at the next row down; the ceiling cursor
  # moved 16 bytes right and must go up one row (-16) instead.
  add  r16, r16, -32
flats_row_done:
  add  r8, r8, 24
  add  r27, r27, 1
  movi r17, 96
  cmp  r27, r17
  bl   flats_row
  add  r2, r2, 1536            # next column group
  add  r7, r7, 4
  add  r25, r25, -1
  bnz  flats_group
  pop  r27
  pop  r26
  pop  r25
  pop  r24
  pop  r23
  pop  r22
  pop  r21
  pop  r20
  ret

# Draw one column of a billboard sprite; palette index 0 is transparent.
# Inputs:  r1 = first destination pixel, r2 = pixel count (> 0),
#          r3 = sprite texel column (SPR_SIZE = 64 bytes), r4 = texel position (16.16),
#          r5 = step per pixel (16.16), r6 = shade table,
#          r7 = rows before the column crosses into the bottom half.
# Outputs: none. Clobbers r1, r2, r4, r7-r9.
  .global draw_sprite_col
draw_sprite_col:
  movi r9, 59904               # HALF_SKIP
draw_sprite_col_loop:
  lsr  r8, r4, 16
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

# Draw one column of a 12-bit overlay image (the staff); pixel 0 is clear.
# Inputs:  r1 = first destination pixel, r2 = pixel count (> 0),
#          r3 = first source pixel (column-major image, so the next row's
#               pixel is the next halfword), r4 = rows before the column
#               crosses into the bottom half.
# Outputs: none. Clobbers r1-r4, r8, r9.
  .global draw_image_col
draw_image_col:
  movi r9, 59904               # HALF_SKIP
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

# Recolor column groups [g0, g1) of the view: every pixel p becomes
# ((p >> shift) & and_mask) | or_mask. Shift 1 with mask 0x777 halves every
# channel (a darkened backdrop); shift 0 tints (the damage flash).
# Inputs:  r1 = back buffer, r2 = g0, r3 = g1 (g0 < g1), r4 = and mask,
#          r5 = or mask (12-bit 0x0BGR values, applied to both pixels of each
#          word), r6 = shift (0 to 15).
# Outputs: none. Clobbers r1-r11.
  .global shade_strip
shade_strip:
  lsl  r7, r4, 16
  or   r4, r4, r7
  lsl  r7, r5, 16
  or   r5, r5, r7
  sub  r3, r3, r2              # column groups in the strip
  mov  r9, r0
  mov  r10, r0
shade_strip_scale:             # r9 = g0 * 1536 bytes, r10 = groups * 384 words
  cmp  r2, r0
  bz   shade_strip_scaled
  movi r7, 1536
  add  r9, r9, r7
  add  r2, r2, -1
  jmp  shade_strip_scale
shade_strip_scaled:
  lsl  r10, r3, 8              # groups * 256
  lsl  r3, r3, 7               # groups * 128
  add  r3, r3, r10             # words per half: 384 per chunk
  add  r1, r1, r9              # the strip's first chunk in the top half
  movi r7, 2                   # halves left
  movi r11, 61440
shade_strip_half:
  mov  r8, r1
  mov  r9, r3
shade_strip_loop:
  lwa  r10, [r8]
  lsr  r10, r10, r6
  and  r10, r10, r4
  or   r10, r10, r5
  swa  r10, [r8], 4
  add  r9, r9, -1
  bnz  shade_strip_loop
  add  r1, r1, r11
  add  r7, r7, -1
  bnz  shade_strip_half
  ret

# Add one wavetable voice into the audio mix accumulator.
# Inputs:  r1 = int accumulator (count words), r2 = sample count (> 0),
#          r3 = wavetable (64 signed halfwords for the voice's volume),
#          r4 = phase (top 6 bits select the table entry), r5 = phase step.
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
