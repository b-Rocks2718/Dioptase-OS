  .text
  .align 4

  .global test_syscall
test_syscall:
  mov  r2, r1
  movi r1, 1
  trap
  ret
