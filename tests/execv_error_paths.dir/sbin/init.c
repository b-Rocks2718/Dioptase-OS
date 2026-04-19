/*
 * execv_error_paths guest:
 * - validate that execv() rejects invalid path and argv inputs and enforces
 *   the current argv count/string-length limits without replacing the caller
 *
 * How:
 * - call execv() with a missing path, a bad path pointer, negative argc, NULL
 *   argv, a bad argv pointer, too many argv entries, and one overlong string
 * - each call must return `-1` to the original process so test_syscall() can
 *   record the failure-path result
 */

#include "../../../crt/print.h"
#include "../../../crt/sys.h"

#define BAD_LOW_PTR ((void*)0x1000)
#define EXEC_LIMIT_PLUS_ONE 17
#define OVERLONG_ARG_BYTES 257

static void fill_overlong_arg(char* buf){
  for (int i = 0; i < OVERLONG_ARG_BYTES - 1; ++i){
    buf[i] = 'A';
  }
  buf[OVERLONG_ARG_BYTES - 1] = '\0';
}

int main(void){
  char overlong_arg[OVERLONG_ARG_BYTES];
  char* too_many_argv[EXEC_LIMIT_PLUS_ONE];
  char* overlong_argv[1];

  fill_overlong_arg(overlong_arg);

  for (int i = 0; i < EXEC_LIMIT_PLUS_ONE; ++i){
    too_many_argv[i] = "x";
  }
  overlong_argv[0] = overlong_arg;

  puts("***missing path\n");
  test_syscall(execv("/test/missing", 0, NULL));

  puts("***bad path pointer\n");
  test_syscall(execv((char*)BAD_LOW_PTR, 0, NULL));

  puts("***negative argc\n");
  test_syscall(execv("/test/init", -1, NULL));

  puts("***null argv\n");
  test_syscall(execv("/test/init", 1, NULL));

  puts("***bad argv pointer\n");
  test_syscall(execv("/test/init", 1, (char**)BAD_LOW_PTR));

  puts("***too many args\n");
  test_syscall(execv("/test/init", EXEC_LIMIT_PLUS_ONE, too_many_argv));

  puts("***long arg\n");
  test_syscall(execv("/test/init", 1, overlong_argv));

  return 0;
}
