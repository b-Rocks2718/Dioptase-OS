  .text
  .align 4

  .define SYNTH_AUDIO_SAMPLE_COUNTER_OFFSET 24
  # 0x80000000 as a signed literal. If (now - target) is below this value,
  # target has been reached in wrap-safe unsigned-counter order.
  .define SYNTH_AUDIO_COUNTER_HALF_RANGE -2147483648

  .global synth_audio_wait_until_sample_counter_asm
synth_audio_wait_until_sample_counter_asm:
  # Purpose: wait until synth MMIO sample counter reaches `target_sample`.
  # Execution mode: user mode; this function performs MMIO loads only.
  # Inputs: r1 = mapped synth MMIO page, r2 = target 32-bit sample counter.
  # Outputs: none.
  # Preconditions:
  # - r1 came from get_synth_audio() and maps the synth audio MMIO page.
  # - The synth device implements version 2 or newer sample-counter semantics.
  # - A single wait is less than 2^31 samples, which is over 23 hours at 25 kHz.
  # ABI notes: docs/abi.md makes r9-r19 caller-saved; this leaf helper only
  # clobbers r9-r11 and does not touch stack, callee-saved registers, or traps.
  add  r9, r1, SYNTH_AUDIO_SAMPLE_COUNTER_OFFSET
  movi r10, SYNTH_AUDIO_COUNTER_HALF_RANGE
synth_audio_wait_until_sample_counter_asm_loop:
  lwa  r11, [r9]
  sub  r11, r11, r2
  cmp  r11, r10
  bae  synth_audio_wait_until_sample_counter_asm_loop
  ret
