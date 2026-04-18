  .text
  .align 4

  .global exit
exit:
  mov  r2, r1
  movi r1, 0
  trap
  ret

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
  mov  r2, r1
  movi r1, 13
  trap
  ret

  .global open
open:
  mov  r2, r1
  movi r1, 14
  trap
  ret

  .global read
read:
  mov  r4, r3
  mov  r3, r2
  mov  r2, r1
  movi r1, 15
  trap
  ret

  .global write
write:
  mov  r4, r3
  mov  r3, r2
  mov  r2, r1
  movi r1, 16
  trap
  ret

  .global close
close:
  mov  r2, r1
  movi r1, 17
  trap
  ret

  .global sem_open
sem_open:
  mov  r2, r1
  movi r1, 18
  trap
  ret

  .global sem_up
sem_up:
  mov  r2, r1
  movi r1, 19
  trap
  ret

  .global sem_down
sem_down:
  mov  r2, r1
  movi r1, 20
  trap
  ret

  .global sem_close
sem_close:
  mov  r2, r1
  movi r1, 21
  trap
  ret

  .global mmap
mmap:
  mov  r5, r4
  mov  r4, r3
  mov  r3, r2
  mov  r2, r1
  movi r1, 22
  trap
  ret

  .global fork
fork:
  movi r1, 23
  trap
  ret

  .global execv
execv:
  mov  r4, r3
  mov  r3, r2
  mov  r2, r1
  movi r1, 24
  trap
  ret

  .global play_audio_file
play_audio_file:
  mov  r2, r1
  movi r1, 25
  trap
  ret

  .global set_text_color
set_text_color:
  mov  r2, r1
  movi r1, 26
  trap
  ret

  .global wait_child
wait_child:
  mov  r2, r1
  movi r1, 27
  trap
  ret

  .global chdir
chdir:
  mov  r2, r1
  movi r1, 28
  trap
  ret

  .global pipe
pipe:
  mov  r2, r1
  movi r1, 29
  trap
  ret

  .global dup
dup:
  mov  r2, r1
  movi r1, 30
  trap
  ret

  .global seek
seek:
  mov  r4, r3
  mov  r3, r2
  mov  r2, r1
  movi r1, 31
  trap
  ret

  .global yield
yield:
  movi r1, 32
  trap
  ret
