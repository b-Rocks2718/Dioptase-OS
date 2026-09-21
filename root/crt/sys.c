#include "sys.h"

// Test syscall list.
void test_syscall_list(int num, int* args){
  for(int i = 0; i < num; i++){
    test_syscall(args[i]);
  }
}
