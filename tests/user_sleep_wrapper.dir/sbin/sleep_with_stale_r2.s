  .text
  .align 4

  .global sleep_with_stale_r2
sleep_with_stale_r2:
  # Save call linkage because this helper makes a nested call into sleep().
  push ra
  push bp
  mov  bp, sp

  # Seed r2 with a value different from the requested sleep length so the
  # wrapper test fails if sleep() forgets to move its C argument into r2 before
  # loading the trap code into r1.
  movi r2, 1
  call sleep

  mov  sp, bp
  lwa  ra, [bp, 4]
  lwa  bp, [bp, 0]
  add  sp, sp, 8
  ret
