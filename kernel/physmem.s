  .text
  .align 4

  .global physmem_metadata_init
  #extern void physmem_metadata_init(struct Page* physmem_map, unsigned frame_count
  #    unsigned flags);
  #// Initialize metadata for every physical frame.
  #for (unsigned i = 0; i < frame_count; i++) {
  #  physmem_map[i].flags = flags;
  #  sem_init(&physmem_map[i].lock, 1);
  #  physmem_map[i].refs = NULL;
  #  physmem_map[i].cache_entry = NULL;
  #}
  # r1 = physmem_map
  # r2 = frame_count
  # r3 = flags
physmem_metadata_init:
  mov  r4, r0 # i = 0
  add  r6, r0, 1 # r6 = 1
  add  r7, r0, 36 # r7 = sizeof(struct Page) = 36
  mov  r5, r1 # r5 = physmem_map

physmem_metadata_init_loop:
  cmp r4, r2 # compare i with frame_count
  bae physmem_metadata_init_end # if i >= frame_count, exit loop

  swa  r3, [r5] # physmem_map[i].flags = flags

  # init refs
  swa r0, [r5, 4] # physmem_map[i].refs = NULL

  # init cache_entry
  swa r0, [r5, 8] # physmem_map[i].cache_entry = NULL

  # init semaphore
  # struct Semaphore {
  #   struct SpinLock lock {
  #      bool the_lock; (init to 0)
  #      int  interrupt_state; (init to 0)
  #   };
  #   int count; (init to 1)
  #   struct Queue wait_queue {
  #     struct TCB* head; (init to NULL)
  #     struct TCB* tail; (init to NULL)
  #     int size; (init to 0)
  #   };
  # };
  # };

  swa r0, [r5, 12] # physmem_map[i].lock.lock.the_lock = 0
  swa r0, [r5, 16] # physmem_map[i].lock.lock.interrupt_state = 0
  swa r6, [r5, 20] # physmem_map[i].lock.count = 1
  swa r0, [r5, 24] # physmem_map[i].lock.wait_queue.head = NULL
  swa r0, [r5, 28] # physmem_map[i].lock.wait_queue.tail = NULL
  swa r0, [r5, 32] # physmem_map[i].lock.wait_queue.size = 0

  add r5, r5, r7 # move to the next Page struct
  add r4, r4, 1 # i++
  jmp physmem_metadata_init_loop

physmem_metadata_init_end:
  ret