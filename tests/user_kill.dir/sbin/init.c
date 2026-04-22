/*
 * user_kill guest:
 * - validate invalid child descriptors return -1 from kill()
 * - validate kill() terminates a fork child before it reaches its normal exit
 *   path
 * - validate wait_child() reports the killed-child result once and then
 *   consumes the descriptor
 *
 * How:
 * - fork one child that repeatedly yields before its normal return so the
 *   parent has time to issue kill() without relying on a one-instruction race
 * - have the parent reject one low and one high invalid descriptor
 * - kill the live child, verify wait_child() reports -1 for the killed child,
 *   then verify the descriptor is consumed for both wait_child() and kill()
 */

#include "../../../root/crt/sys.h"

#define MIN_CHILD_DESCRIPTOR 200
#define MAX_CHILD_DESCRIPTOR_EXCLUSIVE 300
#define INVALID_CHILD_DESCRIPTOR_LOW 199
#define INVALID_CHILD_DESCRIPTOR_HIGH 300

#define CHILD_YIELD_COUNT 256
#define CHILD_SHOULD_NOT_EXIT_STATUS 73

static int child_main(void){
  for (int i = 0; i < CHILD_YIELD_COUNT; ++i){
    yield();
  }

  return CHILD_SHOULD_NOT_EXIT_STATUS;
}

int main(void){
  int child = -1;

  test_syscall(kill(INVALID_CHILD_DESCRIPTOR_LOW));
  test_syscall(kill(INVALID_CHILD_DESCRIPTOR_HIGH));

  child = fork();
  test_syscall(child >= MIN_CHILD_DESCRIPTOR &&
    child < MAX_CHILD_DESCRIPTOR_EXCLUSIVE);

  if (child == 0){
    return child_main();
  }

  test_syscall(kill(child));
  test_syscall(wait_child(child));
  test_syscall(wait_child(child));
  test_syscall(kill(child));

  return 0;
}
