#include "../kernel/print.h"
#include "../kernel/ext.h"
#include "../kernel/vmem.h"
#include "../kernel/elf.h"
#include "../kernel/sys.h"

int kernel_main(void){
  say("Hello from kernel!\n", NULL);

  struct Node* init = node_find(&fs.root, "/sbin/init");
  unsigned size = node_size_in_bytes(init);
  unsigned* prog = mmap(size, init, 0, MMAP_READ);

  say("Mapped /sbin/init.c into memory\n", NULL);

  unsigned entry = elf_load(prog);
  say("Loaded ELF, entry point at 0x%x\n", &entry);

  unsigned* stack = mmap(0x4000, NULL, 0, MMAP_READ | MMAP_WRITE | MMAP_USER);
  say("Mapped user stack at 0x%x\n", &stack);

  int rc = jump_to_user(entry, (unsigned)stack + 0x3FFC);
  say("User returned %d\n", &rc);

  node_free(init);

  return 42;
}
