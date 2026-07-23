/*
 * user_foreground_display guest:
 * - verify signal_foreground() validates and forwards its signal argument
 * - verify display traps from the foreground child's parent do not claim the
 *   foreground descriptor
 * - verify a display trap from the live foreground child is reported when the
 *   foreground slot is cleared after child exit
 *
 * How:
 * - each child waits on a shared semaphore until the parent has installed it as
 *   the foreground child, avoiding scheduler-dependent races
 * - the first child remains blocked while the parent sends an invalid signal
 *   and changes tile scale; it then exits without using a display trap
 * - the second child changes tile scale itself before exiting
 * - wait_child() consumes the parent's descriptor-table reference before each
 *   clear, exercising the foreground slot's independent descriptor reference
 */

#include "../../../root/crt/sys.h"

#define INVALID_SIGNAL 32
#define TERMINAL_TILE_SCALE 0
#define CHILD_FAILURE_STATUS 73
#define CLOSED_GATE_COUNT 0

static int wait_for_parent(int gate){
  if (sem_down(gate) != 0){
    return CHILD_FAILURE_STATUS;
  }
  if (sem_close(gate) != 0){
    return CHILD_FAILURE_STATUS;
  }
  return 0;
}

static void test_non_child_display_use(void){
  int gate = sem_open(CLOSED_GATE_COUNT);
  int child = fork();

  if (child == 0){
    exit(wait_for_parent(gate));
  }

  test_syscall(set_foreground_child(child));
  test_syscall(signal_foreground(INVALID_SIGNAL));

  // The parent is not the TCB named by the foreground descriptor, so this
  // direct display trap must not claim recovery on behalf of the child.
  set_tile_scale(TERMINAL_TILE_SCALE);

  sem_up(gate);
  test_syscall(wait_child(child));
  test_syscall(set_foreground_child(-1));
  sem_close(gate);
}

static void test_foreground_child_display_use(void){
  int gate = sem_open(CLOSED_GATE_COUNT);
  int child = fork();

  if (child == 0){
    int rc = wait_for_parent(gate);
    if (rc != 0){
      exit(rc);
    }
    set_tile_scale(TERMINAL_TILE_SCALE);
    exit(0);
  }

  test_syscall(set_foreground_child(child));
  sem_up(gate);
  test_syscall(wait_child(child));
  test_syscall(set_foreground_child(-1));
  sem_close(gate);
}

int main(void){
  test_non_child_display_use();
  test_foreground_child_display_use();
  return 0;
}
