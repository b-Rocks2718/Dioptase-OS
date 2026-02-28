# the compiler generated code for this was too slow

# this just copies the sprite animations into the sprite data

  .align 4
  .data

DINO_FRAME:
  .fill 0

SUN_FRAME:
  .fill 0

  .align 4
  .text

  .global do_animations
do_animations:
  # sun animations
  adpc r3, SUNSHEET_DATA
  adpc r4, SUN_FRAME
  lwa  r4, [r4]
  movi r6, 2048 # bytes per 32x32 sprite frame (16bpp)
  
do_animations_loop_1:
  cmp  r4, r0
  bz   12
  add  r3, r3, r6
  add  r4, r4, -1
  jmp  do_animations_loop_1
  
  adpc r4, SPRITE_DATA_START
  lwa  r4, [r4]
  add  r4, r4, r6
  add  r4, r4, r6
  add  r4, r4, r6

do_animations_loop_2:
  lda  r5, [r3]
  # if value is 0xF3F, instead write 0xF000
  movi r7, 0xF3F
  cmp  r5, r7
  bnz  do_animations_loop_2_skip
  movi r5, 0xF000
do_animations_loop_2_skip:
  sda  r5, [r4]
  add  r3, r3, 2
  add  r4, r4, 2
  # lda/sda move 16-bit pixels, so count down by bytes to copy exactly 2048 bytes.
  add  r6, r6, -2
  bnz  do_animations_loop_2
  adpc r3, SUN_FRAME
  lwa  r4, [r3]
  add  r4, r4, 1
  movi r5, 3
  cmp  r4, r5
  bl   8
  movi r4, 0
  swa  r4, [r3]

  # dino animations
  adpc r3, DINORUNSHEET_DATA
  adpc r4, DINO_FRAME
  lwa  r4, [r4]
  movi r6, 2048 # bytes per 32x32 sprite frame (16bpp)

do_animations_loop_3:
  cmp  r4, r0
  bz   12
  add  r3, r3, r6
  add  r4, r4, -1
  jmp  do_animations_loop_3
  
  adpc r4, SPRITE_DATA_START
  # SPRITE_DATA_START is a 32-bit pointer variable; load the full word.
  lwa  r4, [r4]
  add  r4, r4, r6
  add  r4, r4, r6
do_animations_loop_4:
  lda  r5, [r3]
  # if value is 0xF3F, instead write 0xF000
  movi r7, 0xF3F
  cmp  r5, r7
  bnz  do_animations_loop_4_skip
  movi r5, 0xF000
do_animations_loop_4_skip:
  sda  r5, [r4]
  add  r3, r3, 2
  add  r4, r4, 2
  # lda/sda move 16-bit pixels, so count down by bytes to copy exactly 2048 bytes.
  add  r6, r6, -2
  bnz  do_animations_loop_4
  adpc r3, DINO_FRAME
  lwa  r4, [r3]
  add  r4, r4, 1
  movi r5, 6
  cmp  r4, r5
  bl   8
  movi r4, 0
  swa  r4, [r3]

  ret
