  .text
  .align 4

  .global bmacs_present_viewport_asm
bmacs_present_viewport_asm:
  # Purpose:
  # - Present the prepared 56x80 character viewport into the mapped tile
  #   framebuffer, including the top bar plus the three-line footer.
  #
  # Inputs:
  # - r1 = tile framebuffer base pointer (`short*`)
  # - r2 = render_rows base pointer (`char*`)
  #
  # Outputs:
  # - none
  #
  # Preconditions:
  # - `r1` points at the 80x60 tile framebuffer returned by `get_tile_fb()`
  # - `r2` points at exactly 56 contiguous rows of 80 character bytes
  #
  # ABI notes:
  # - Uses only caller-saved temporaries (r3-r11)

  # Draw the top border row with light-gray square tiles.
  movi r3 0x927F
  mov  r4 r1
  movi r5 80
bmacs_present_top_border_loop:
  cmp  r5 r0
  bz   bmacs_present_top_border_done
  sda  r3, [r4]
  add  r4 r4 2
  add  r5 r5 -1
  jmp  bmacs_present_top_border_loop
bmacs_present_top_border_done:

  # Draw the visible text area starting at framebuffer row 1
  # (1 * 80 * 2 = 160 bytes).
  movi r6 160
  add  r4 r1 r6
  mov  r6 r2
  movi r7 56
  movi r11 0xFF00
bmacs_present_row_loop:
  cmp  r7 r0
  bz   bmacs_present_footer_top_row
  movi r5 80
bmacs_present_col_loop:
  cmp  r5 r0
  bz   bmacs_present_row_done
  lba  r8, [r6]
  movi r9 0xFF
  and  r8 r8 r9
  or   r8 r8 r11
  sda  r8, [r4]
  add  r6 r6 1
  add  r4 r4 2
  add  r5 r5 -1
  jmp  bmacs_present_col_loop
bmacs_present_row_done:
  add  r7 r7 -1
  jmp  bmacs_present_row_loop

bmacs_present_footer_top_row:
  movi r5 80
bmacs_present_footer_top_loop:
  cmp  r5 r0
  bz   bmacs_present_status_row
  sda  r3, [r4]
  add  r4 r4 2
  add  r5 r5 -1
  jmp  bmacs_present_footer_top_loop

bmacs_present_status_row:
  movi r10 0xFF20
  movi r5 80
bmacs_present_status_loop:
  cmp  r5 r0
  bz   bmacs_present_bottom_border_row
  sda  r10, [r4]
  add  r4 r4 2
  add  r5 r5 -1
  jmp  bmacs_present_status_loop

bmacs_present_bottom_border_row:
  movi r5 80
bmacs_present_bottom_border_loop:
  cmp  r5 r0
  bz   bmacs_present_done
  sda  r3, [r4]
  add  r4 r4 2
  add  r5 r5 -1
  jmp  bmacs_present_bottom_border_loop

bmacs_present_done:
  ret
