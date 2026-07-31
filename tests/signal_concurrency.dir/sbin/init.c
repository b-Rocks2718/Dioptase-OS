/*
 * Signal concurrency guest test:
 *
 * Behavior under test:
 * - reverse-order pending signals are delivered in numeric priority order
 * - a nonmaskable signal received during a handler terminates without nesting
 * - a synchronous fault inside a handler terminates without nesting
 * - mask/unmask operations remain coherent with concurrent cross-core sends
 * - sending SIGNAL_KILL while a child concurrently exits never accesses a
 *   stale TCB and produces only outcomes allowed by descriptor publication
 *
 * Why this exists:
 * Signal senders, scheduler delivery, user handlers, and child exit execute on
 * different kernel activations and may run on different cores. Functional
 * tests with fully ordered semaphore handshakes do not exercise the lifetime
 * and state-transition windows between those operations.
 *
 * How it works:
 * Semaphores establish only the minimum lifecycle preconditions. The target
 * and sender then yield or continue independently so mask changes, sends, and
 * exit publication overlap. Each subtest reports one deterministic result;
 * timing-dependent outcomes are checked as a valid set rather than encoded in
 * the golden output.
 */

#include "../../../root/crt/sys.h"
#include "../../user_test.h"

/*
 * Each run uses both scheduling shapes in test_send_exit_race(): four races
 * release and signal immediately, while four yield to the exiting child first.
 * The synchronization creates the cross-core overlap; larger counts only
 * repeat those same shapes and make the multicore emulator prohibitively slow.
 */
#define STRESS_ITERATIONS 8
#define DELIVERY_WAIT_YIELDS 32

#define NORMAL_EXIT_STATUS 73
#define NESTED_HANDLER_STATUS 91

#define ORDER_FAILURE_STATUS 1
#define MASK_RACE_FAILURE_STATUS 2

extern void trigger_invalid_instruction(void);

static int ready_sem = -1;
static int continue_sem = -1;
static int handler_entered_sem = -1;
static int sends_done_sem = -1;

static int delivered_count = 0;
static int delivery_order[2] = {-1, -1};

static int recording_handler(int signal){
  if (delivered_count < 2){
    delivery_order[delivered_count] = signal;
  }
  delivered_count += 1;
  sigreturn(0);
}

static int blocking_handler(int signal){
  sem_up(handler_entered_sem);
  while (1){
    yield();
  }
}

static int forbidden_nested_handler(int signal){
  exit(NESTED_HANDLER_STATUS);
}

static int faulting_handler(int signal){
  trigger_invalid_instruction();
  exit(NESTED_HANDLER_STATUS);
}

static int counting_handler(int signal){
  delivered_count += 1;
  sigreturn(0);
}

static int pending_order_child(void){
  if (register_handler(SIGNAL_HELLO, (void*)recording_handler) != 0 ||
      register_handler(SIGNAL_TERMINATE, (void*)recording_handler) != 0 ||
      mask_signal(SIGNAL_HELLO) != 0 ||
      mask_signal(SIGNAL_TERMINATE) != 0){
    return ORDER_FAILURE_STATUS;
  }

  sem_up(ready_sem);
  sem_down(continue_sem);

  if (unmask_signal(SIGNAL_HELLO) != 0 ||
      unmask_signal(SIGNAL_TERMINATE) != 0){
    return ORDER_FAILURE_STATUS;
  }

  for (int i = 0; i < DELIVERY_WAIT_YIELDS && delivered_count < 2; ++i){
    yield();
  }

  if (delivered_count != 2 ||
      delivery_order[0] != SIGNAL_HELLO ||
      delivery_order[1] != SIGNAL_TERMINATE){
    return ORDER_FAILURE_STATUS;
  }
  return 0;
}

static int active_handler_child(void){
  if (register_handler(SIGNAL_HELLO, (void*)blocking_handler) != 0 ||
      register_handler(SIGNAL_ILL, (void*)forbidden_nested_handler) != 0){
    return NESTED_HANDLER_STATUS;
  }

  sem_up(ready_sem);
  while (1){
    yield();
  }
}

static int fault_in_handler_child(void){
  if (register_handler(SIGNAL_HELLO, (void*)faulting_handler) != 0 ||
      register_handler(SIGNAL_ILL, (void*)forbidden_nested_handler) != 0){
    return NESTED_HANDLER_STATUS;
  }

  sem_up(ready_sem);
  while (1){
    yield();
  }
}

static int mask_send_race_child(void){
  if (register_handler(SIGNAL_HELLO, (void*)counting_handler) != 0){
    return MASK_RACE_FAILURE_STATUS;
  }

  for (int i = 0; i < STRESS_ITERATIONS; ++i){
    if (mask_signal(SIGNAL_HELLO) != 0){
      return MASK_RACE_FAILURE_STATUS;
    }

    // The parent is released while this signal is masked. Yielding here gives
    // another core a chance to publish the pending bit concurrently with the
    // child's following unmask operation.
    sem_up(ready_sem);
    yield();

    if (unmask_signal(SIGNAL_HELLO) != 0){
      return MASK_RACE_FAILURE_STATUS;
    }
    yield();
  }

  // Keep child_tcb live until the parent confirms every send completed.
  sem_down(sends_done_sem);
  unmask_signal(SIGNAL_HELLO);
  for (int i = 0; i < DELIVERY_WAIT_YIELDS && delivered_count == 0; ++i){
    yield();
  }

  if (delivered_count <= 0 || delivered_count > STRESS_ITERATIONS){
    return MASK_RACE_FAILURE_STATUS;
  }
  return 0;
}

static int exiting_child(void){
  sem_up(ready_sem);
  sem_down(continue_sem);
  return NORMAL_EXIT_STATUS;
}

static int test_pending_order(void){
  ready_sem = sem_open(0);
  continue_sem = sem_open(0);

  int child = fork();
  if (child == 0){
    exit(pending_order_child());
    return -1;
  }

  sem_down(ready_sem);
  int failures = 0;
  failures += signal_child(child, SIGNAL_TERMINATE) != 0;
  failures += signal_child(child, SIGNAL_HELLO) != 0;
  sem_up(continue_sem);
  failures += wait_child(child) != 0;

  sem_close(ready_sem);
  sem_close(continue_sem);
  return failures;
}

static int test_nonmaskable_during_handler(void){
  ready_sem = sem_open(0);
  handler_entered_sem = sem_open(0);

  int child = fork();
  if (child == 0){
    exit(active_handler_child());
    return -1;
  }

  sem_down(ready_sem);
  int failures = signal_child(child, SIGNAL_HELLO) != 0;
  sem_down(handler_entered_sem);
  failures += signal_child(child, SIGNAL_ILL) != 0;
  failures += wait_child(child) != -1;

  sem_close(ready_sem);
  sem_close(handler_entered_sem);
  return failures;
}

static int test_fault_during_handler(void){
  ready_sem = sem_open(0);

  int child = fork();
  if (child == 0){
    exit(fault_in_handler_child());
    return -1;
  }

  sem_down(ready_sem);
  int failures = signal_child(child, SIGNAL_HELLO) != 0;
  failures += wait_child(child) != -1;

  sem_close(ready_sem);
  return failures;
}

static int test_mask_send_race(void){
  ready_sem = sem_open(0);
  sends_done_sem = sem_open(0);

  int child = fork();
  if (child == 0){
    exit(mask_send_race_child());
    return -1;
  }

  int failures = 0;
  for (int i = 0; i < STRESS_ITERATIONS; ++i){
    sem_down(ready_sem);
    failures += signal_child(child, SIGNAL_HELLO) != 0;
  }
  sem_up(sends_done_sem);
  failures += wait_child(child) != 0;

  sem_close(ready_sem);
  sem_close(sends_done_sem);
  return failures;
}

static int test_send_exit_race(void){
  ready_sem = sem_open(0);
  continue_sem = sem_open(0);
  int failures = 0;

  for (int i = 0; i < STRESS_ITERATIONS; ++i){
    int child = fork();
    if (child == 0){
      exit(exiting_child());
      return -1;
    }

    sem_down(ready_sem);
    sem_up(continue_sem);

    // Odd iterations let the child run first; even iterations leave the
    // sender and exiting child racing immediately on separate cores.
    if ((i & 1) != 0){
      yield();
    }

    int signal_rc = signal_child(child, SIGNAL_KILL);
    int wait_rc = wait_child(child);

    if (signal_rc != 0 && signal_rc != -1){
      failures += 1;
    }
    if (wait_rc != -1 && wait_rc != NORMAL_EXIT_STATUS){
      failures += 1;
    }
    // Once exit publication makes signal_child() fail, the already-published
    // result must be the child's normal status rather than signal termination.
    if (signal_rc == -1 && wait_rc != NORMAL_EXIT_STATUS){
      failures += 1;
    }
  }

  sem_close(ready_sem);
  sem_close(continue_sem);
  return failures;
}

int main(void){
  user_test_expect_eq("test_pending_order()", test_pending_order(), 0);
  user_test_expect_eq("test_nonmaskable_during_handler()", test_nonmaskable_during_handler(), 0);
  user_test_expect_eq("test_fault_during_handler()", test_fault_during_handler(), 0);
  user_test_expect_eq("test_mask_send_race()", test_mask_send_race(), 0);
  user_test_expect_eq("test_send_exit_race()", test_send_exit_race(), 0);
  return 0;
}
