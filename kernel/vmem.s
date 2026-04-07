  .text
  .align 4

  .global tlb_kmiss_handler_
tlb_kmiss_handler_:
  mov r1, tlb
  call tlb_kmiss_handler
  ret

  .global tlb_umiss_handler_
tlb_umiss_handler_:
  mov r1, tlb
  call tlb_umiss_handler
  ret

