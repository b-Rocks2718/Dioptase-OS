  .text
  .align 4

  .global heap_large_alloc_init
heap_large_alloc_init:
  # extern void heap_large_alloc_init(unsigned char* large_allocation_orders,
  #   int phys_frame_count, int heap_large_alloc_none);

  # for (int i = 0; i < PHYS_FRAME_COUNT; i++) {
  #  large_allocation_orders[i] = HEAP_LARGE_ALLOC_NONE;
  # }

  # r1 = large_allocation_orders
  # r2 = phys_frame_count
  # r3 = heap_large_alloc_none

  mov r4, r0 # i = 0
loop:
  cmp r4, r2 # i < phys_frame_count
  bge done

  sba r3, [r1] # large_allocation_orders[i] = heap_large_alloc_none

  add r1, r1, 1 # move to next byte
  add r4, r4, 1 # i++
  jmp loop
done:
  ret
