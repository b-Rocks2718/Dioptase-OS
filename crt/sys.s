  .text
  .align 4

  .global test_syscall
test_syscall:
  # Trap ABI uses r2-r8 for trap arguments, so preserve the C arg before
  # loading the trap code into r1.
  mov  r2, r1
  movi r1, 1
  trap
  ret

  .global get_current_jiffies
get_current_jiffies:
  movi r1, 2
  trap
  ret

  .global getkey
getkey:
  movi r1, 3
  trap
  ret

  .global set_tile_scale
set_tile_scale:
  # Trap ABI uses r2-r8 for trap arguments.
  mov  r2, r1
  movi r1, 4
  trap
  ret

  .global set_vscroll
set_vscroll:
  # Trap ABI uses r2-r8 for trap arguments.
  mov  r2, r1
  movi r1, 5
  trap
  ret

  .global set_hscroll
set_hscroll:
  # Trap ABI uses r2-r8 for trap arguments.
  mov  r2, r1
  movi r1, 6
  trap
  ret

  .global load_text_tiles
load_text_tiles:
  movi r1, 7
  trap
  ret

  .global clear_screen
clear_screen:
  movi r1, 8
  trap
  ret

  .global get_tilemap
get_tilemap:
  movi r1, 9
  trap
  ret

  .global get_tile_fb
get_tile_fb:
  movi r1, 10
  trap
  ret

  .global get_vga_status
get_vga_status:
  movi r1, 11
  trap
  ret

  .global get_vga_frame_counter
get_vga_frame_counter:
  movi r1, 12
  trap
  ret

  .global sleep
sleep:
  # Trap ABI uses r2-r8 for trap arguments.
  mov  r2, r1
  movi r1, 13
  trap
  ret
