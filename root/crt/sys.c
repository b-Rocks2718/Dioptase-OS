#include "sys.h"

// Invoke the test syscall once for each supplied argument.
void test_syscall_list(int num, int* args){
  for(int i = 0; i < num; i++){
    test_syscall(args[i]);
  }
}
