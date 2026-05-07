#include "../../../root/crt/sys.h"

int main(void){
  int x = test_syscall(1);
  test_syscall(x);

  int list[2] = {6, 7};
  test_syscall_list(2, list);

  return 42;
}
