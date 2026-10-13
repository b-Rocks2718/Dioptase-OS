  .text
  .align 4

# Publish one row of the powder world into the pixel framebuffer.
# Each cell is a 2x2 pixel block, so one row of colors fills two
# framebuffer rows; the duplicated color is packed into a single word
# store per row instead of two halfword stores (docs/mem_map.md).
#
# Inputs:
# - r1 = framebuffer word pointer for this cell row (320 shorts per row)
# - r2 = colors, short[160], the row's packed 0x0BGR colors
# - r3 = prev, short[160], last published colors (updated in place)
#
# Outputs:
# - none
#
# Preconditions:
# - r1 is 4-byte aligned and rows r1 and r1+640 bytes are writable
# - Only caller-saved registers r4-r11 are clobbered.
  .global powder_publish_row
powder_publish_row:
  mov  r4, r1
  movi r5, 640
  add  r5, r1, r5
  mov  r6, r2
  mov  r7, r3
  movi r8, 160
powder_publish_loop:
  cmp  r8, r0
  bz   powder_publish_done
  lda  r9, [r6], 2
  lda  r10, [r7]
  cmp  r9, r10
  bz   powder_publish_skip
  sda  r9, [r7]
  lsl  r11, r9, 16
  or   r11, r11, r9
  swa  r11, [r4]
  swa  r11, [r5]
powder_publish_skip:
  add  r4, r4, 4
  add  r5, r5, 4
  add  r7, r7, 2
  add  r8, r8, -1
  jmp  powder_publish_loop
powder_publish_done:
  ret
