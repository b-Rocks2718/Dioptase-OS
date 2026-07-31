#include "../../../root/crt/sys.h"
#include "../../../root/crt/stdio.h"
#include "../../../root/crt/print.h"

int child_barrier = -1;
int hello_barrier = -1;

static int hello_handler(void){
  puts("***Child received HELLO signal\n");

  // signal the parent that the child has received the HELLO signal
  sem_up(hello_barrier);

  sigreturn(0);
}

static int seg_handler(void* vpn){
  unsigned addr = (unsigned)vpn << 12;
  printf("***Child received SEG signal at address 0x%X\n", &addr);
  exit(1);
}

static int child_main(void){
  puts("***Child waiting for signals\n");
  sem_up(child_barrier);
  while (1); // child exists only to run signal handlers
}

int main(void){
  int child = -1;

  child_barrier = sem_open(0);
  hello_barrier = sem_open(0);

  puts("***Registering HELLO signal handler\n");
  register_handler(SIGNAL_HELLO, (void*)hello_handler);
  puts("***Registered HELLO signal handler\n");
  
  child = fork();
  if (child == 0){
    return child_main();
  }

  sem_down(child_barrier); // wait for child to be ready

  puts("***Signaling child with HELLO signal\n");
  signal_child(child, SIGNAL_HELLO); // should get handled

  // wait for the child to receive the HELLO signal
  sem_down(hello_barrier);

  puts("***Signaling child with HELLO signal (again)\n");
  signal_child(child, SIGNAL_HELLO); // should get handled

  // wait for the child to receive the HELLO signal
  sem_down(hello_barrier);

  puts("***Signaling child with TERMINATE signal\n");
  signal_child(child, SIGNAL_TERMINATE); // no handler in place; should kill child

  sem_close(child_barrier);
  sem_close(hello_barrier);

  puts("***Registering SEG signal handler\n");
  register_handler(SIGNAL_SEG, (void*)seg_handler);

  puts("***Testing SIGNAL_SEG with a segmentation fault\n");
  int* bad_ptr = (int*)0x12345678;
  *bad_ptr = 42; // should get handled by seg_handler

  return 0;
}
