#include "../../../root/crt/sys.h"
#include "lib.h"

int main(void){ /* Verify the second get-to-user userland fixture. */
  int y = lib_function(3, 4);
  test_syscall(y);

  int x = test_syscall(1);
  test_syscall(x);

  int list[2] = {6, 7};
  test_syscall_list(2, list);

  return 42;
}
