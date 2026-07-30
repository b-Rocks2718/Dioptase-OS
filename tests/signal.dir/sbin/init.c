#include "../../../root/crt/sys.h"
#include "../../../root/crt/stdio.h"

int hello_barrier = -1;

static int child_hello_handler(void){
  puts("***Child received HELLO signal\n");

  // signal the parent that the child has received the HELLO signal
  sem_up(hello_barrier);

  sigreturn(0);
}

static int child_main(void){
  register_handler(SIGNAL_HELLO, (void*)child_hello_handler);
  while (1); // child exists only to run signal handlers
}

int main(void){
  int child = -1;

  hello_barrier = sem_open(0);
  
  child = fork();
  if (child == 0){
    return child_main();
  }

  puts("***Signaling child with HELLO signal\n");
  signal_child(child, SIGNAL_HELLO); // should get handled

  // wait for the child to receive the HELLO signal
  sem_down(hello_barrier);

  puts("***Signaling child with TERMINATE signal\n");
  signal_child(child, SIGNAL_TERMINATE); // no handler in place; should kill child

  sem_close(hello_barrier);

  return 0;
}
