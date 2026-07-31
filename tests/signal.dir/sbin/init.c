/*
 * Signal guest test:
 * - validates handler-registration, masking, unmasking, and sigreturn errors
 * - verifies maskable asynchronous signals remain pending, coalesce while
 *   masked, receive their signal number, and resume after sigreturn()
 * - verifies an asynchronous handler can terminate with an explicit status
 * - verifies SIGNAL_SEG, SIGNAL_ILL, and SIGNAL_ALGN are delivered with their
 *   documented fault arguments, including both invalid and privileged
 *   instruction causes for SIGNAL_ILL
 * - verifies unhandled asynchronous and synchronous signals terminate a child
 * - verifies exec clears handler addresses inherited from the replaced image
 *
 * Each terminating case runs in a separate fork child so one fault cannot
 * prevent the remaining cases from running. Semaphores establish the
 * parent/child ordering for asynchronous delivery without timing assumptions.
 */

#include "../../../root/crt/sys.h"

#define CHILD_READY_STATUS 41
#define ASYNC_EXIT_STATUS 42
#define SEG_EXIT_STATUS 43
#define ILL_EXIT_STATUS 44
#define ALGN_EXIT_STATUS 45
#define EXEC_UNEXPECTED_RETURN_STATUS 46

#define DECIMAL_HUNDREDS 100
#define DECIMAL_TENS 10
#define HIGHEST_MASKABLE_SIGNAL 15

extern void trigger_invalid_instruction(void);
extern void trigger_privileged_instruction(void);
extern void trigger_misaligned_pc(void);

static int masked_hello_count = 0;
static int child_ready_sem = -1;
static int child_continue_sem = -1;

static int masked_hello_handler(int signal){
  masked_hello_count += 1;
  test_syscall(signal);
  sigreturn(0);
}

static int async_exit_handler(int signal){
  test_syscall(signal);
  exit(ASYNC_EXIT_STATUS);
}

static int seg_handler(void* vpn){
  test_syscall((int)vpn);
  exit(SEG_EXIT_STATUS);
}

static int ill_handler(unsigned pc){
  test_syscall(pc & 3);
  exit(ILL_EXIT_STATUS);
}

static int algn_handler(unsigned pc){
  test_syscall(pc & 3);
  exit(ALGN_EXIT_STATUS);
}

static int masked_child(void){
  test_syscall(register_handler(SIGNAL_HELLO,
    (void*)masked_hello_handler));
  test_syscall(mask_signal(SIGNAL_HELLO));
  sem_up(child_ready_sem);
  sem_down(child_continue_sem);

  // Both parent sends happened while masked, so the handler must not have run.
  test_syscall(masked_hello_count);
  test_syscall(unmask_signal(SIGNAL_HELLO));

  // Delivery happens at a scheduling boundary. The pending bitmap coalesces
  // both sends into one handler invocation.
  yield();
  test_syscall(masked_hello_count);
  return CHILD_READY_STATUS;
}

static int waiting_child(void){
  sem_up(child_ready_sem);
  while (1){
    yield();
  }
}

static int parse_three_digit_decimal(char* text){
  return (text[0] - '0') * DECIMAL_HUNDREDS +
    (text[1] - '0') * DECIMAL_TENS +
    (text[2] - '0');
}

static int exec_check_main(char* ready_sem_text){
  int ready_sem = parse_three_digit_decimal(ready_sem_text);
  sem_up(ready_sem);
  while (1){
    yield();
  }
}

int main(int argc, char** argv){
  if (argc == 3 && argv[1][0] == 'e'){
    return exec_check_main(argv[2]);
  }

  int child = -1;
  int nonexec_handler = 0;

  // API validation, including executable handler-address checking and the
  // distinction between maskable and nonmaskable signals.
  test_syscall(sigreturn(0));
  test_syscall(register_handler(-1, (void*)masked_hello_handler));
  test_syscall(register_handler(32, (void*)masked_hello_handler));
  test_syscall(register_handler(SIGNAL_KILL,
    (void*)masked_hello_handler));
  test_syscall(register_handler(SIGNAL_HELLO, (void*)&nonexec_handler));
  test_syscall(register_handler(SIGNAL_HELLO,
    (void*)((unsigned)masked_hello_handler + 2)));
  test_syscall(mask_signal(-1));
  test_syscall(mask_signal(MAX_MASKABLE_SIGNAL));
  test_syscall(mask_signal(SIGNAL_ILL));
  test_syscall(mask_signal(SIGNAL_KILL));
  test_syscall(unmask_signal(SIGNAL_ALGN));
  test_syscall(mask_signal(HIGHEST_MASKABLE_SIGNAL));
  test_syscall(unmask_signal(HIGHEST_MASKABLE_SIGNAL));
  test_syscall(mask_signal(SIGNAL_HELLO));
  test_syscall(mask_signal(SIGNAL_HELLO));
  test_syscall(unmask_signal(SIGNAL_HELLO));
  test_syscall(unmask_signal(SIGNAL_HELLO));

  child_ready_sem = sem_open(0);
  child_continue_sem = sem_open(0);

  child = fork();
  if (child == 0){
    return masked_child();
  }
  sem_down(child_ready_sem);
  test_syscall(signal_child(child, SIGNAL_HELLO));
  test_syscall(signal_child(child, SIGNAL_HELLO));
  sem_up(child_continue_sem);
  test_syscall(wait_child(child));

  child = fork();
  if (child == 0){
    test_syscall(register_handler(SIGNAL_HELLO,
      (void*)async_exit_handler));
    return waiting_child();
  }
  sem_down(child_ready_sem);
  test_syscall(signal_child(child, SIGNAL_HELLO));
  test_syscall(wait_child(child));

  // An unhandled, unmasked asynchronous signal uses the default termination
  // action and reports -1 to wait_child().
  child = fork();
  if (child == 0){
    return waiting_child();
  }
  sem_down(child_ready_sem);
  test_syscall(signal_child(child, SIGNAL_TERMINATE));
  test_syscall(wait_child(child));

  // SIGNAL_KILL takes the same default termination action and cannot be
  // intercepted, regardless of the target's other registered handlers.
  child = fork();
  if (child == 0){
    return waiting_child();
  }
  sem_down(child_ready_sem);
  test_syscall(signal_child(child, SIGNAL_KILL));
  test_syscall(wait_child(child));

  child = fork();
  if (child == 0){
    test_syscall(register_handler(SIGNAL_SEG, (void*)seg_handler));
    int* bad_ptr = (int*)0x12345678;
    *bad_ptr = 42;
    return -1;
  }
  test_syscall(wait_child(child));

  child = fork();
  if (child == 0){
    test_syscall(register_handler(SIGNAL_ILL, (void*)ill_handler));
    trigger_invalid_instruction();
    return -1;
  }
  test_syscall(wait_child(child));

  child = fork();
  if (child == 0){
    test_syscall(register_handler(SIGNAL_ILL, (void*)ill_handler));
    trigger_privileged_instruction();
    return -1;
  }
  test_syscall(wait_child(child));

  child = fork();
  if (child == 0){
    test_syscall(register_handler(SIGNAL_ALGN, (void*)algn_handler));
    trigger_misaligned_pc();
    return -1;
  }
  test_syscall(wait_child(child));

  child = fork();
  if (child == 0){
    trigger_invalid_instruction();
    return -1;
  }
  test_syscall(wait_child(child));

  child = fork();
  if (child == 0){
    trigger_privileged_instruction();
    return -1;
  }
  test_syscall(wait_child(child));

  child = fork();
  if (child == 0){
    trigger_misaligned_pc();
    return -1;
  }
  test_syscall(wait_child(child));

  child = fork();
  if (child == 0){
    test_syscall(register_handler(SIGNAL_HELLO,
      (void*)async_exit_handler));

    // Semaphore descriptors occupy 100..199, so every valid value has exactly
    // three decimal digits. Pass the inherited descriptor to the replacement
    // image without relying on its reset global variables.
    char ready_sem_text[4];
    ready_sem_text[0] = '0' + child_ready_sem / DECIMAL_HUNDREDS;
    ready_sem_text[1] =
      '0' + (child_ready_sem / DECIMAL_TENS) % DECIMAL_TENS;
    ready_sem_text[2] = '0' + child_ready_sem % DECIMAL_TENS;
    ready_sem_text[3] = 0;

    char* exec_argv[4] = {
      "/sbin/init",
      "exec-check",
      ready_sem_text,
      NULL
    };
    execv("/sbin/init", 3, exec_argv);
    return EXEC_UNEXPECTED_RETURN_STATUS;
  }
  sem_down(child_ready_sem);
  test_syscall(signal_child(child, SIGNAL_HELLO));
  test_syscall(wait_child(child));

  sem_close(child_ready_sem);
  sem_close(child_continue_sem);
  return 0;
}
