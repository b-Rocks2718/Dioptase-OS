  .text
  .align 4

  # Opcode 23 is reserved by the ISA. This is used to verify that a
  # synchronous SIGNAL_ILL raised inside an asynchronous handler terminates
  # instead of nesting a second user handler.
  .global trigger_invalid_instruction
trigger_invalid_instruction:
  .fill 0xB8000000
  ret
