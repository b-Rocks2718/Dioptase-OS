  .text
  .align 4

  .global audio_copy_even_bytes_to_ring_asm
audio_copy_even_bytes_to_ring_asm:
  # Purpose:
  # - Copy an even number of signed-16 PCM bytes into the fixed MMIO audio
  #   ring, handling wrap-around internally.
  #
  # Inputs:
  # - r1 = src pointer
  # - r2 = current AUDIO_WRITE_IDX
  # - r3 = copy_bytes (even)
  #
  # Outputs:
  # - r1 = new AUDIO_WRITE_IDX after copying `copy_bytes`
  #
  # Preconditions:
  # - src and the ring destination selected by write_idx are both 2-byte aligned
  # - copy_bytes is even
  #
  # ABI notes:
  # - Uses only caller-saved temporaries (r4-r10)
  movi r4 0x4000
  sub  r5 r4 r2
  cmp  r5 r3
  bbe  audio_copy_even_first_ready
  mov  r5 r3
audio_copy_even_first_ready:
  movi r6 0x7FB8000
  add  r7 r6 r2
  mov  r8 r5
audio_copy_even_first_loop:
  cmp  r8 r0
  bz   audio_copy_even_first_done
  lda  r9, [r1]
  sda  r9, [r7]
  add  r1 r1 2
  add  r7 r7 2
  add  r8 r8 -2
  jmp  audio_copy_even_first_loop
audio_copy_even_first_done:
  sub  r8 r3 r5
  mov  r7 r6
audio_copy_even_second_loop:
  cmp  r8 r0
  bz   audio_copy_even_finish
  lda  r9, [r1]
  sda  r9, [r7]
  add  r1 r1 2
  add  r7 r7 2
  add  r8 r8 -2
  jmp  audio_copy_even_second_loop
audio_copy_even_finish:
  add  r2 r2 r3
  cmp  r2 r4
  bb   audio_copy_even_no_wrap
  sub  r2 r2 r4
audio_copy_even_no_wrap:
  mov  r1 r2
  ret

  .global audio_copy_word_bytes_to_ring_asm
audio_copy_word_bytes_to_ring_asm:
  # Purpose:
  # - Copy a multiple of 4 PCM bytes into the fixed MMIO audio ring, handling
  #   wrap-around internally.
  #
  # Inputs:
  # - r1 = src pointer
  # - r2 = current AUDIO_WRITE_IDX
  # - r3 = copy_bytes (multiple of 4)
  #
  # Outputs:
  # - r1 = new AUDIO_WRITE_IDX after copying `copy_bytes`
  #
  # Preconditions:
  # - src and the ring destination selected by write_idx are both 4-byte aligned
  # - copy_bytes is a multiple of 4
  #
  # ABI notes:
  # - Uses only caller-saved temporaries (r4-r10)
  movi r4 0x4000
  sub  r5 r4 r2
  cmp  r5 r3
  bbe  audio_copy_word_first_ready
  mov  r5 r3
audio_copy_word_first_ready:
  movi r6 0x7FB8000
  add  r7 r6 r2
  mov  r8 r5
audio_copy_word_first_loop:
  cmp  r8 r0
  bz   audio_copy_word_first_done
  lwa  r9, [r1]
  swa  r9, [r7]
  add  r1 r1 4
  add  r7 r7 4
  add  r8 r8 -4
  jmp  audio_copy_word_first_loop
audio_copy_word_first_done:
  sub  r8 r3 r5
  mov  r7 r6
audio_copy_word_second_loop:
  cmp  r8 r0
  bz   audio_copy_word_finish
  lwa  r9, [r1]
  swa  r9, [r7]
  add  r1 r1 4
  add  r7 r7 4
  add  r8 r8 -4
  jmp  audio_copy_word_second_loop
audio_copy_word_finish:
  add  r2 r2 r3
  cmp  r2 r4
  bb   audio_copy_word_no_wrap
  sub  r2 r2 r4
audio_copy_word_no_wrap:
  mov  r1 r2
  ret

  .global mark_audio_handled
mark_audio_handled:
  eoi 7
  ret

  # Audio interrupt entry point
  .global audio_handler_
audio_handler_:
  push  r1
  push  r2
  push  r3
  push  r4
  push  r5
  push  r6
  push  r7
  push  r8
  push  r9
  push  r10
  push  r11
  push  r12
  push  r13
  push  r14
  push  r15
  push  r16
  push  r17
  push  r18
  push  r19

  # Save epc/efg using r1 as scratch (r1 already saved).
  mov  r1, epc
  push r1
  mov  r1, efg
  push r1

  # Save bp and ra.
  push bp
  push ra

  call audio_handler

  pop  ra
  pop  bp

  pop  r1
  mov  efg, r1
  pop  r1
  mov  epc, r1

  pop  r19
  pop  r18
  pop  r17
  pop  r16
  pop  r15
  pop  r14
  pop  r13
  pop  r12
  pop  r11
  pop  r10
  pop  r9
  pop  r8
  pop  r7
  pop  r6
  pop  r5
  pop  r4
  pop  r3
  pop  r2
  pop  r1

  rfe
  