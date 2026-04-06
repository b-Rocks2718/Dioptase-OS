  # Software arithmetic helpers used by compiler-generated code.

  .text
  .align 4

  .global smul
smul:
  # signed 32-bit multiply implemented in software.
  # Inputs: r1, r2.
  # Outputs: r1 = product modulo 2^32.
  # Algorithm:
  # - Count how many operands are negative in r4.
  # - Negate negative inputs so the main loop can operate on magnitudes.
  # - Perform classic shift/add multiplication on those magnitudes.
  lui r3 0x80000000
  mov r4 r0

  # Normalize r1 to a non-negative magnitude and remember whether we flipped it.
  and r0 r1 r3
  bz  smul_check_r2
  add r4 r4 1
  sub r1 r0 r1
smul_check_r2:
  # Normalize r2 in the same way.
  and r0 r2 r3
  bz  smul_abs
  add r4 r4 1
  sub r2 r0 r2
smul_abs:
  # Loop state:
  # - r3 = accumulated product
  # - r5 = remaining multiplier bits
  # - r6 = current multiplicand contribution
  mov r3 r0
  mov r5 r2
  mov r6 r1
smul_loop:
  cmp r5 r0
  bz  smul_done

  # If bit 0 of the multiplier is set, add in the current multiplicand.
  and r7 r5 1
  bz  smul_skip_add
  add r3 r3 r6
smul_skip_add:
  # Advance to the next multiplier bit.
  lsl r6 r6 1
  lsr r5 r5 1
  jmp smul_loop
smul_done:
  # Exactly one negative input means the final result must be negated.
  add r4 r4 -1
  bnz smul_skip_negate
  sub r3 r0 r3
smul_skip_negate:
  mov r1 r3
  ret

  .global sdiv
sdiv:
  # signed 32-bit divide implemented in software.
  # Inputs: r1 = dividend, r2 = divisor.
  # Outputs: r1 = quotient truncated toward zero.
  # Zero-divisor convention: return 0.
  # Algorithm:
  # - Convert dividend and divisor to unsigned magnitudes.
  # - Count negative operands in r4 so the quotient sign can be restored.
  # - Use binary long division: align the divisor with the dividend, then walk
  #   the divisor back down while setting quotient bits in r3.
  cmp r2 r0
  bz  sdiv_divzero
  lui r3 0x80000000
  mov r4 r0

  # Normalize the dividend and record whether it was negative.
  and r0 r1 r3
  bz  sdiv_check_r2
  add r4 r4 1
  sub r1 r0 r1
sdiv_check_r2:
  # Normalize the divisor and record whether it was negative.
  and r0 r2 r3
  bz  sdiv_abs
  add r4 r4 1
  sub r2 r0 r2
sdiv_abs:
  # Loop state:
  # - r3 = quotient being built
  # - r5 = current trial divisor
  # - r6 = remaining dividend / remainder
  # - r7 = bit index associated with r5
  mov r3 r0
  mov r5 r2
  mov r6 r1
  mov r7 r0
sdiv_align:
  # Stop growing the divisor once it already has bit 31 set; another shift
  # would leave the non-negative magnitude range we are using here.
  cmp r5 r0
  bs  sdiv_loop

  # r1 is scratch here: test whether doubling the divisor still keeps it
  # unsigned <= the dividend magnitude.
  lsl r1 r5 1
  cmp r1 r6
  bbe sdiv_shift
  jmp sdiv_loop
sdiv_shift:
  mov r5 r1
  add r7 r7 1
  jmp sdiv_align
sdiv_loop:
  # When r7 becomes negative, all quotient bits have been considered.
  cmp r7 r0
  bs  sdiv_done

  # If the current trial divisor still fits, subtract it and set that quotient
  # bit. Otherwise leave both unchanged and try the next smaller bit.
  cmp r6 r5
  bb  sdiv_skip_sub
  sub r6 r6 r5
  mov r1 r0
  add r1 r1 1
  lsl r1 r1 r7
  or  r3 r3 r1
sdiv_skip_sub:
  lsr r5 r5 1
  add r7 r7 -1
  jmp sdiv_loop
sdiv_done:
  mov r1 r3

  # Exactly one negative input means the quotient must be negated.
  add r4 r4 -1
  bnz sdiv_skip_negate
  sub r1 r0 r1
sdiv_skip_negate:
  ret
sdiv_divzero:
  mov r1 r0
  ret

  .global smod
smod:
  # signed 32-bit remainder implemented in software.
  # Inputs: r1 = dividend, r2 = divisor.
  # Outputs: r1 = remainder whose sign matches the dividend.
  # Zero-divisor convention: return 0.
  # Algorithm: the long-division core matches sdiv, but we keep only the final
  # remainder instead of the quotient bits.
  cmp r2 r0
  bz  smod_divzero
  lui r3 0x80000000
  mov r8 r0

  # Only the dividend sign matters for the final remainder sign, but both
  # operands still need to be normalized so the unsigned division loop works.
  and r0 r1 r3
  bz  smod_check_r2
  add r8 r8 1
  sub r1 r0 r1
smod_check_r2:
  and r0 r2 r3
  bz  smod_abs
  sub r2 r0 r2
smod_abs:
  # Loop state:
  # - r4 = current trial divisor
  # - r5 = running remainder
  # - r6 = bit index associated with r4
  mov r4 r2
  mov r5 r1
  mov r6 r0
smod_align:
  cmp r4 r0
  bs  smod_loop
  lsl r7 r4 1
  cmp r7 r5
  bbe smod_shift
  jmp smod_loop
smod_shift:
  mov r4 r7
  add r6 r6 1
  jmp smod_align
smod_loop:
  cmp r6 r0
  bs  smod_done
  cmp r5 r4
  bb  smod_skip_sub
  sub r5 r5 r4
smod_skip_sub:
  lsr r4 r4 1
  add r6 r6 -1
  jmp smod_loop
smod_done:
  mov r1 r5

  # Restore the dividend sign to the remainder.
  add r8 r8 -1
  bnz smod_skip_negate
  sub r1 r0 r1
smod_skip_negate:
  ret
smod_divzero:
  mov r1 r0
  ret

  .global umul
umul:
  # unsigned 32-bit multiply implemented in software.
  # Inputs: r1 = multiplicand, r2 = multiplier.
  # Outputs: r1 = product modulo 2^32.
  # Algorithm: identical to smul after sign normalization has been skipped.
  mov r3 r0
  mov r5 r2
  mov r6 r1
umul_loop:
  cmp r5 r0
  bz  umul_end
  and r7 r5 1
  bz  umul_skip_add
  add r3 r3 r6
umul_skip_add:
  lsl r6 r6 1
  lsr r5 r5 1
  jmp umul_loop
umul_end:
  mov r1 r3
  ret

  .global udiv
udiv:
  # unsigned 32-bit divide implemented in software.
  # Inputs: r1 = dividend, r2 = divisor.
  # Outputs: r1 = quotient.
  # Zero-divisor convention: return 0.
  # Algorithm: binary long division with an alignment pass followed by a
  # subtract/shift pass.
  cmp r2 r0
  bz  udiv_divzero
  mov r3 r0
  mov r4 r2
  mov r5 r1
  mov r6 r0
udiv_align:
  cmp r4 r0
  bs  udiv_loop
  lsl r7 r4 1
  cmp r7 r5
  bbe udiv_shift
  jmp udiv_loop
udiv_shift:
  mov r4 r7
  add r6 r6 1
  jmp udiv_align
udiv_loop:
  cmp r6 r0
  bs  udiv_done
  cmp r5 r4
  bb  udiv_skip_sub
  sub r5 r5 r4
  mov r7 r0
  add r7 r7 1
  lsl r7 r7 r6
  or  r3 r3 r7
udiv_skip_sub:
  lsr r4 r4 1
  add r6 r6 -1
  jmp udiv_loop
udiv_done:
  mov r1 r3
  ret
udiv_divzero:
  mov r1 r0
  ret

  .global umod
umod:
  # unsigned 32-bit remainder implemented in software.
  # Inputs: r1 = dividend, r2 = divisor.
  # Outputs: r1 = remainder.
  # Zero-divisor convention: return 0.
  # Algorithm: same alignment/subtract loop as udiv, but keep only the
  # remaining dividend in r5.
  cmp r2 r0
  bz  umod_divzero
  mov r4 r2
  mov r5 r1
  mov r6 r0
umod_align:
  cmp r4 r0
  bs  umod_loop
  lsl r7 r4 1
  cmp r7 r5
  bbe umod_shift
  jmp umod_loop
umod_shift:
  mov r4 r7
  add r6 r6 1
  jmp umod_align
umod_loop:
  cmp r6 r0
  bs  umod_done
  cmp r5 r4
  bb  umod_skip_sub
  sub r5 r5 r4
umod_skip_sub:
  lsr r4 r4 1
  add r6 r6 -1
  jmp umod_loop
umod_done:
  mov r1 r5
  ret
umod_divzero:
  mov r1 r0
  ret

  # The four shift entry points below intentionally share loop labels inside
  # each signed/unsigned pair. Negative counts redirect into the opposite loop
  # after negating the count, so the common single-bit shift logic lives in one
  # place instead of being duplicated.

  .global sleft_shift
sleft_shift:
  # Purpose: software signed left shift.
  # Inputs: r1 = value, r2 = shift count.
  # Outputs: r1 = value shifted by |r2| one-bit steps.
  # Local convention:
  # - r2 >= 0: arithmetic left shift (implemented with logical left shifts).
  # - r2 <  0: redirect to signed right shift by -r2.
  # Counts are not masked to 5 bits; this loop executes exactly |r2| shifts.
  lui r3 0x80000000
  and r0 r3 r2
  bz  sls_loop
  sub r2 r0 r2
  jmp srs_loop
sls_loop:
  # Shared loop body for sleft_shift and the negative-count path of
  # sright_shift.
  cmp r2 r0
  bz  sls_end
  add r2 r2 -1
  lsl r1 r1 1
  jmp sls_loop
sls_end:
  ret

  .global sright_shift
sright_shift:
  # Purpose: software signed right shift.
  # Inputs: r1 = value, r2 = shift count.
  # Outputs: r1 = arithmetic-right-shifted value after |r2| one-bit steps.
  # Local convention:
  # - r2 >= 0: arithmetic right shift.
  # - r2 <  0: redirect to left shift by -r2.
  lui r3 0x80000000
  and r0 r3 r2
  bz  srs_loop
  sub r2 r0 r2
  jmp sls_loop
srs_loop:
  # Shared loop body for sright_shift and the negative-count path of
  # sleft_shift.
  cmp r2 r0
  bz  srs_end
  add r2 r2 -1
  asr r1 r1 1
  jmp srs_loop
srs_end:
  ret

  .global uleft_shift
uleft_shift:
  # Purpose: software unsigned left shift.
  # Inputs: r1 = value, r2 = shift count.
  # Outputs: r1 = value shifted by |r2| one-bit steps.
  # Local convention:
  # - r2 >= 0: logical left shift.
  # - r2 <  0: redirect to logical right shift by -r2.
  lui r3 0x80000000
  and r0 r3 r2
  bz  uls_loop
  sub r2 r0 r2
  jmp urs_loop
uls_loop:
  # Shared loop body for uleft_shift and the negative-count path of
  # uright_shift.
  cmp r2 r0
  bz  uls_end
  add r2 r2 -1
  lsl r1 r1 1
  jmp uls_loop
uls_end:
  ret

  .global uright_shift
uright_shift:
  # Purpose: software unsigned right shift.
  # Inputs: r1 = value, r2 = shift count.
  # Outputs: r1 = logically-right-shifted value after |r2| one-bit steps.
  # Local convention:
  # - r2 >= 0: logical right shift.
  # - r2 <  0: redirect to left shift by -r2.
  lui r3 0x80000000
  and r0 r3 r2
  bz  urs_loop
  sub r2 r0 r2
  jmp uls_loop
urs_loop:
  # Shared loop body for uright_shift and the negative-count path of
  # uleft_shift.
  cmp r2 r0
  bz  urs_end
  add r2 r2 -1
  lsr r1 r1 1
  jmp urs_loop
urs_end:
  ret
