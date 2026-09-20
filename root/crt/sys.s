  .text
  .align 4

  .global exit
# Marshal arguments and issue the exit system-call wrapper.
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
# Marshal arguments and issue the get current jiffies system-call wrapper.
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
# Marshal arguments and issue the getkey system-call wrapper.
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
# Marshal arguments and issue the set tile scale system-call wrapper.
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
# Marshal arguments and issue the set vscroll system-call wrapper.
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
# Marshal arguments and issue the set hscroll system-call wrapper.
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
# Marshal arguments and issue the load text tiles system-call wrapper.
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
# Marshal arguments and issue the clear screen system-call wrapper.
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
# Marshal arguments and issue the get tilemap system-call wrapper.
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
# Marshal arguments and issue the get tile fb system-call wrapper.
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
# Marshal arguments and issue the get VGA status system-call wrapper.
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
# Marshal arguments and issue the get VGA frame counter system-call wrapper.
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
# Marshal arguments and issue the sleep system-call wrapper.
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
# Marshal arguments and issue the open system-call wrapper.
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

  .global open_existing
# Marshal arguments and issue the open existing system-call wrapper.
open_existing:
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

  # The C ABI supplies pathname in r1. Move it to the trap ABI's first
  # argument register before selecting Dioptase-OS trap code 56. `trap` enters
  # kernel mode; its ordinary return restores user mode and leaves the fd/-1
  # result in r1.
  mov  r2, r1
  movi r1, 56
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
# Marshal arguments and issue the read system-call wrapper.
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
# Marshal arguments and issue the write system-call wrapper.
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
# Marshal arguments and issue the close system-call wrapper.
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
# Marshal arguments and issue the sem open system-call wrapper.
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
# Marshal arguments and issue the sem up system-call wrapper.
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
# Marshal arguments and issue the sem down system-call wrapper.
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
# Marshal arguments and issue the sem close system-call wrapper.
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
# Marshal arguments and issue the mmap system-call wrapper.
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
# Marshal arguments and issue the fork system-call wrapper.
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
# Marshal arguments and issue the execv system-call wrapper.
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
# Marshal arguments and issue the play audio file system-call wrapper.
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
# Marshal arguments and issue the set text color system-call wrapper.
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
# Marshal arguments and issue the wait child system-call wrapper.
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
# Marshal arguments and issue the chdir system-call wrapper.
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
# Marshal arguments and issue the pipe system-call wrapper.
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
# Marshal arguments and issue the dup system-call wrapper.
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
# Marshal arguments and issue the seek system-call wrapper.
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
# Marshal arguments and issue the yield system-call wrapper.
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
# Marshal arguments and issue the getdents system-call wrapper.
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
# Marshal arguments and issue the getcwd system-call wrapper.
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
# Marshal arguments and issue the readlink system-call wrapper.
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

  .global move_vscroll
# Marshal arguments and issue the move vscroll system-call wrapper.
move_vscroll:
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
  movi r1, 36
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

  .global move_hscroll
# Marshal arguments and issue the move hscroll system-call wrapper.
move_hscroll:
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
  movi r1, 37
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
  
  .global fd_bytes_available
# Marshal arguments and issue the fd bytes available system-call wrapper.
fd_bytes_available:
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
  movi r1, 38
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

  .global truncate
# Marshal arguments and issue the truncate system-call wrapper.
truncate:
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
  movi r1, 39
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

  .global mkdir
# Marshal arguments and issue the mkdir system-call wrapper.
mkdir:
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
  movi r1, 40
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

  .global rmdir
# Marshal arguments and issue the rmdir system-call wrapper.
rmdir:
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
  movi r1, 41
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

.global unlink
# Marshal arguments and issue the unlink system-call wrapper.
unlink:
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
  movi r1, 42
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

  .global set_sprite_scale
# Marshal arguments and issue the set sprite scale system-call wrapper.
set_sprite_scale:
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
  movi r1, 43
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

  .global set_sprite_coords
# Marshal arguments and issue the set sprite coords system-call wrapper.
set_sprite_coords:
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
  movi r1, 44
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

  .global load_text_tiles_colored
# Marshal arguments and issue the load text tiles colored system-call wrapper.
load_text_tiles_colored:
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
  movi r1, 45
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

  .global get_spritemap
# Marshal arguments and issue the get spritemap system-call wrapper.
get_spritemap:
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

  movi r1, 46
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

  .global signal_child
# Marshal arguments and issue the signal child system-call wrapper.
signal_child:
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
  movi r1, 47
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

  .global request_priority
# Marshal arguments and issue the request priority system-call wrapper.
request_priority:
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
  movi r1, 49
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

  .global set_foreground_child
# Marshal arguments and issue the set foreground child system-call wrapper.
set_foreground_child:
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
  movi r1, 50
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

  .global signal_foreground
# Marshal arguments and issue the signal foreground system-call wrapper.
signal_foreground:
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
  movi r1, 51
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

  .global register_handler
# Marshal arguments and issue the register handler system-call wrapper.
register_handler:
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
  movi r1, 52
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

  .global sigreturn
sigreturn:
  # A valid sigreturn does not resume this wrapper, but an invalid call made
  # outside a handler returns -1 through the ordinary trap path. Keep the
  # wrapper's return address in one trap-callee-saved register. Only that
  # register needs a stack save: a valid sigreturn abandons this user stack,
  # while the invalid path restores both r20 and ra before returning.
  push r20
  mov  r20, ra

  mov  r2, r1
  movi r1, 53
  trap

  mov ra, r20
  pop r20

  ret # only returns with -1 when called outside a signal handler

  .global mask_signal
# Marshal arguments and issue the mask signal system-call wrapper.
mask_signal:
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
  movi r1, 54
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

  .global unmask_signal
# Marshal arguments and issue the unmask signal system-call wrapper.
unmask_signal:
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
  movi r1, 55
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
