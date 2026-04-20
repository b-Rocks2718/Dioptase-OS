  .text
  .align 4

  .global exit
exit:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r2, r1
  movi r1, 0
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global test_syscall
test_syscall:
  # Trap ABI uses r2-r8 for trap arguments, so preserve the C arg before
  # loading the trap code into r1.
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r2, r1
  movi r1, 1
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global get_current_jiffies
get_current_jiffies:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  movi r1, 2
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global getkey
getkey:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  movi r1, 3
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global set_tile_scale
set_tile_scale:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  # Trap ABI uses r2-r8 for trap arguments.
  mov  r2, r1
  movi r1, 4
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global set_vscroll
set_vscroll:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  # Trap ABI uses r2-r8 for trap arguments.
  mov  r2, r1
  movi r1, 5
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global set_hscroll
set_hscroll:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  # Trap ABI uses r2-r8 for trap arguments.
  mov  r2, r1
  movi r1, 6
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global load_text_tiles
load_text_tiles:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  movi r1, 7
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global clear_screen
clear_screen:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  movi r1, 8
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global get_tilemap
get_tilemap:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  movi r1, 9
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global get_tile_fb
get_tile_fb:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  movi r1, 10
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global get_vga_status
get_vga_status:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  movi r1, 11
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global get_vga_frame_counter
get_vga_frame_counter:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  movi r1, 12
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global sleep
sleep:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r2, r1
  movi r1, 13
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global open
open:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r2, r1
  movi r1, 14
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global read
read:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r4, r3
  mov  r3, r2
  mov  r2, r1
  movi r1, 15
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global write
write:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r4, r3
  mov  r3, r2
  mov  r2, r1
  movi r1, 16
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global close
close:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r2, r1
  movi r1, 17
  trap
  
  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global sem_open
sem_open:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r2, r1
  movi r1, 18
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global sem_up
sem_up:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r2, r1
  movi r1, 19
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20
  
  ret

  .global sem_down
sem_down:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r2, r1
  movi r1, 20
  trap
  
  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global sem_close
sem_close:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r2, r1
  movi r1, 21
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global mmap
mmap:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r5, r4
  mov  r4, r3
  mov  r3, r2
  mov  r2, r1
  movi r1, 22
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global fork
fork:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  movi r1, 23
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global execv
execv:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r4, r3
  mov  r3, r2
  mov  r2, r1
  movi r1, 24
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global play_audio_file
play_audio_file:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r2, r1
  movi r1, 25
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20
  
  ret

  .global set_text_color
set_text_color:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r2, r1
  movi r1, 26
  trap
  
  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global wait_child
wait_child:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r2, r1
  movi r1, 27
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20
  ret

  .global chdir
chdir:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r2, r1
  movi r1, 28
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global pipe
pipe:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r2, r1
  movi r1, 29
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global dup
dup:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r2, r1
  movi r1, 30
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global seek
seek:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r4, r3
  mov  r3, r2
  mov  r2, r1
  movi r1, 31
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global yield
yield:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  movi r1, 32
  trap
  
  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global getdents
getdents:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r4, r3
  mov  r3, r2
  mov  r2, r1
  movi r1, 33
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global getcwd
getcwd:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r3, r2
  mov  r2, r1
  movi r1, 34
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret

  .global readlink
readlink:
  push r20
  push r21
  push r22
  push r23
  push r24
  push r25
  push r26
  push r27
  push r28
  push bp
  push ra

  mov  r4, r3
  mov  r3, r2
  mov  r2, r1
  movi r1, 35
  trap

  pop ra
  pop bp
  pop r28
  pop r27
  pop r26
  pop r25
  pop r24
  pop r23
  pop r22
  pop r21
  pop r20

  ret
