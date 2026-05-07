    
  .text
  .align 4
  
  .global __atomic_exchange_n
__atomic_exchange_n:
  # atomic exchange: swap value in r1 with value in r2
  # put old value in r1
  swpa r1, r2, [r1]
  ret

  .global __atomic_fetch_add
__atomic_fetch_add:
  # atomic fetch add: add value in r2 to value stored at r1
  # put old value in r1
  fada r1, r2, [r1]
  ret

  .global __atomic_load_n
__atomic_load_n:
  # atomic load: load value stored at r1 into r1
  lwa r1, [r1]
  ret

  .global __atomic_store_n
__atomic_store_n:
  # atomic store: store value in r2 to address in r1
  swa r2, [r1]
  ret
  
