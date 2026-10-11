# DELVE hand-written inner loops (user mode).
#
# These are the per-pixel and per-column routines that dominate a frame. They
# are plain leaf functions under the Dioptase ABI (docs/abi.md): arguments in
# r1-r8, result in r1, and they only touch caller-saved r1-r19, so they need
# no stack frame. None of them trap or touch privileged state.
#
# Back-buffer layout (see delve.h, "View layout"): the 160x96 view is stored
# exactly like the 240 tiles that display it, tile (tx, ty) at tile index
# tx * 12 + ty. Pixel (x, y) therefore lives at byte offset
#   (x >> 3) * 1536 + (x & 7) * 2 + y * 16
# so walking down a screen column is a constant 16-byte stride, and the
# finished frame reaches tile memory with one straight copy.

  .text
  .align 4

# Copy the finished 160x96 view (30720 bytes) into the tilemap.
# Inputs:  r1 = destination (mapped tilemap), r2 = source back buffer.
#          Both word aligned.
# Outputs: none. Clobbers r1-r12.
  .global blit_view
blit_view:
  movi r3, 960                # 30720 bytes / 32 bytes per iteration
blit_view_loop:
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
  add  r3, r3, -1
  bnz  blit_view_loop
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
#          r5 = texture step per pixel (16.16), r6 = shade table (256 halfwords).
# Outputs: none. Clobbers r1, r2, r4, r7, r8.
  .global draw_wall
draw_wall:
  lsr  r7, r4, 16
  add  r7, r3, r7
  lba  r7, [r7]
  lsl  r7, r7, 1
  add  r7, r6, r7
  lda  r8, [r7]
  sda  r8, [r1], 16
  add  r4, r4, r5
  add  r2, r2, -1
  bnz  draw_wall
  ret

# Fill part of a view column with per-row colors (flat floor/ceiling shading).
# Inputs:  r1 = first destination pixel, r2 = pixel count (> 0),
#          r3 = pointer to the first row's 16-bit color; the table advances
#          by r4 bytes per pixel (2 walks down, -2 walks up).
# Outputs: none. Clobbers r1-r3, r5.
  .global fill_rows
fill_rows:
  lda  r5, [r3]
  add  r3, r3, r4
  sda  r5, [r1], 16
  add  r2, r2, -1
  bnz  fill_rows
  ret

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

# Cast the floor and ceiling over the whole view (perspective-correct floor
# casting: along a screen row the texture coordinates advance linearly).
#
# Pixels are visited one 8-pixel-wide tile column at a time (all 48 row pairs
# of one 1536-byte group before the next) so the back-buffer working set is a
# single page; walking whole screen rows instead touches eight pages per row
# and thrashes the 16-entry TLB.
#
# Inputs:  r1 = 48 FlatRow records (delve.h), one per floor row k, holding the
#               row's current (u, v), per-pixel (du, dv) and shade tables.
#               u and v are advanced in place across the tile columns.
#          r2 = back buffer (view pixel (0, 0)).
#          r3 = floor texture, r4 = ceiling texture (64x64, row-major).
# Outputs: none. Clobbers r1-r19; preserves r20-r21 (saved on the stack).
  .global draw_flats_asm
draw_flats_asm:
  push r20
  push r21
  movi r5, 0xFC0               # mask for (v >> 4): texel row * 64
  movi r6, 20                  # tile columns
flats_group:
  mov  r8, r1                  # FlatRow cursor
  movi r7, 48                  # row pairs
  movi r15, 768                # floor row 48 at byte offset 48 * 16
  add  r15, r2, r15
  movi r16, 752                # ceiling row 47 at byte offset 47 * 16
  add  r16, r2, r16
flats_row:
  lwa  r9, [r8, 0]             # u
  lwa  r10, [r8, 4]            # v
  lwa  r11, [r8, 8]            # du
  lwa  r12, [r8, 12]           # dv
  lwa  r13, [r8, 16]           # floor shade table
  lwa  r14, [r8, 20]           # ceiling shade table
  movi r17, 8
flats_pixel:
  lsr  r18, r9, 10
  and  r18, r18, 63
  lsr  r19, r10, 4
  and  r19, r19, r5
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
  add  r8, r8, 24
  # The floor cursor already sits at the next row down; the ceiling cursor
  # moved 16 bytes right and must go up one row (-16) instead.
  add  r16, r16, -32
  add  r7, r7, -1
  bnz  flats_row
  movi r21, 1536
  add  r2, r2, r21
  add  r6, r6, -1
  bnz  flats_group
  pop  r21
  pop  r20
  ret
