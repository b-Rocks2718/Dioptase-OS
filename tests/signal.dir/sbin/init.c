/*
 * Signal guest test:
 * - validates handler-registration, masking, unmasking, and sigreturn errors
 * - verifies maskable asynchronous signals remain pending, coalesce while
 *   masked, receive their signal number, and resume after sigreturn()
 * - verifies an asynchronous handler can terminate with an explicit status
 * - verifies handler entry preserves PIT delivery so a syscall-free handler
 *   remains preemptible by an asynchronously sent nonmaskable signal
 * - verifies SIGNAL_SEG, SIGNAL_ILL, and SIGNAL_ALGN are delivered with their
 *   documented fault arguments, including both invalid and privileged
 *   instruction causes for SIGNAL_ILL
 * - verifies sigreturn from a synchronous handler retries the retained fault
 *   frame instead of unwinding the interrupted user program
 * - verifies unhandled asynchronous and synchronous signals terminate a child
 * - verifies exec clears handler addresses inherited from the replaced image
 *
 * Each terminating case runs in a separate fork child so one fault cannot
 * prevent the remaining cases from running. Semaphores establish the
 * parent/child ordering for asynchronous delivery without timing assumptions.
 */

#include "../../../root/crt/sys.h"
#include "../../user_test.h"

#define CHILD_READY_STATUS 41
#define ASYNC_EXIT_STATUS 42
#define SEG_EXIT_STATUS 43
#define ILL_EXIT_STATUS 44
#define ALGN_EXIT_STATUS 45
#define EXEC_UNEXPECTED_RETURN_STATUS 46
#define ILL_RETRY_EXIT_STATUS 47

#define DECIMAL_HUNDREDS 100
#define DECIMAL_TENS 10
#define HIGHEST_MASKABLE_SIGNAL 15
#define SEG_FAULT_ADDRESS 0x12345678
#define EXPECTED_SEG_FAULT_VPN 0x12345

extern void trigger_invalid_instruction(void);
extern void trigger_privileged_instruction(void);
extern void trigger_misaligned_pc(void);

static int masked_hello_count = 0;
static int ill_retry_count = 0;
static int child_ready_sem = -1;
static int child_continue_sem = -1;

static int masked_hello_handler(int signal){ /* Verify masked delivery by recording the deferred hello signal. */
  masked_hello_count += 1;
  user_test_expect_eq("masked handler signal number", signal, SIGNAL_HELLO);
  sigreturn(0);
}

static int async_exit_handler(int signal){ /* Record asynchronous delivery and request the test process exit. */
  user_test_expect_eq("terminating handler signal number", signal,
    SIGNAL_HELLO);
  exit(ASYNC_EXIT_STATUS);
}

static int spinning_handler(int signal){ /* Keep the handler busy to exercise signal interruption behavior. */
  if (signal != SIGNAL_HELLO){
    exit(-1);
  }

  // Publish that the handler itself is active, then make no further syscall.
  // Only a PIT final-return boundary can observe the later SIGNAL_KILL. If
  // handler entry loses the lower IMR device bits, this loop never terminates.
  sem_up(child_ready_sem);
  while (1){ }
}

static int seg_handler(void* vpn){ /* Record the page-fault address delivered with the signal. */
  user_test_expect_eq("segmentation handler fault VPN", (int)vpn,
    EXPECTED_SEG_FAULT_VPN);
  exit(SEG_EXIT_STATUS);
}

static int ill_handler(unsigned pc){ /* Record the illegal-instruction address delivered by the kernel. */
  user_test_expect_eq("illegal-instruction handler PC alignment", pc & 3, 0);
  exit(ILL_EXIT_STATUS);
}

static int ill_retry_handler(unsigned pc){ /* Record an illegal instruction while requesting the retry path. */
  if ((pc & 3) != 0){
    exit(-1);
  }

  ill_retry_count += 1;
  if (ill_retry_count == 1){
    // The invalid instruction remains unchanged. A correct sigreturn restores
    // the exception wrapper's saved frame and faults at the same EPC again.
    sigreturn(0);
  }

  exit(ill_retry_count == 2 ? ILL_RETRY_EXIT_STATUS : -1);
}

static int algn_handler(unsigned pc){ /* Record the address of the misaligned instruction fault. */
  user_test_expect_eq("misaligned-PC handler PC remainder", pc & 3, 2);
  exit(ALGN_EXIT_STATUS);
}

static int masked_child(void){ /* Run the masked child process. */
  user_test_expect_eq("register masked hello handler",
    register_handler(SIGNAL_HELLO, (void*)masked_hello_handler), 0);
  user_test_expect_eq("mask hello in child", mask_signal(SIGNAL_HELLO), 0);
  sem_up(child_ready_sem);
  sem_down(child_continue_sem);

  // Both parent sends happened while masked, so the handler must not have run.
  user_test_expect_eq("masked hello delivery count before unmask",
    masked_hello_count, 0);

  // Preserve the result without printing it yet. Once the signal is unmasked,
  // a PIT final-return boundary may enter the pending handler between any two
  // writes made by user_test_expect_eq(), even though both reports belong to
  // this thread.
  int unmask_rc = unmask_signal(SIGNAL_HELLO);

  // Delivery happens at an explicit final kernel-to-user return boundary. The
  // pending bitmap coalesces both sends into one handler invocation. Once
  // yield returns, the handler's report is complete and the remaining reports
  // cannot interleave with it.
  yield();
  user_test_expect_eq("unmask pending hello", unmask_rc, 0);
  user_test_expect_eq("coalesced hello delivery count after unmask",
    masked_hello_count, 1);
  return CHILD_READY_STATUS;
}

static int waiting_child(void){ /* Run the waiting child process. */
  sem_up(child_ready_sem);
  while (1){
    yield();
  }
}

static int parse_three_digit_decimal(char* text){ /* Parse three digit decimal. */
  return (text[0] - '0') * DECIMAL_HUNDREDS +
    (text[1] - '0') * DECIMAL_TENS +
    (text[2] - '0');
}

static int exec_check_main(char* ready_sem_text){ /* Report post-exec readiness, then remain available for signaling. */
  int ready_sem = parse_three_digit_decimal(ready_sem_text);
  sem_up(ready_sem);
  while (1){
    yield();
  }
}

int main(int argc, char** argv){ /* Exercise basic signal delivery and masking. */
  if (argc == 3 && argv[1][0] == 'e'){
    return exec_check_main(argv[2]);
  }

  int child = -1;
  int nonexec_handler = 0;

  // API validation, including executable handler-address checking and the
  // distinction between maskable and nonmaskable signals.
  user_test_expect_eq("sigreturn outside a handler", sigreturn(0), -1);
  user_test_expect_eq("reject negative handler signal",
    register_handler(-1, (void*)masked_hello_handler), -1);
  user_test_expect_eq("reject out-of-range handler signal",
    register_handler(32, (void*)masked_hello_handler), -1);
  user_test_expect_eq("reject handler for SIGNAL_KILL",
    register_handler(SIGNAL_KILL, (void*)masked_hello_handler), -1);
  user_test_expect_eq("reject non-executable handler address",
    register_handler(SIGNAL_HELLO, (void*)&nonexec_handler), -1);
  user_test_expect_eq("reject misaligned handler address",
    register_handler(SIGNAL_HELLO,
      (void*)((unsigned)masked_hello_handler + 2)), -1);
  user_test_expect_eq("reject negative mask signal", mask_signal(-1), -1);
  user_test_expect_eq("reject first nonmaskable signal",
    mask_signal(MAX_MASKABLE_SIGNAL), -1);
  user_test_expect_eq("reject masking SIGNAL_ILL",
    mask_signal(SIGNAL_ILL), -1);
  user_test_expect_eq("reject masking SIGNAL_KILL",
    mask_signal(SIGNAL_KILL), -1);
  user_test_expect_eq("reject unmasking SIGNAL_ALGN",
    unmask_signal(SIGNAL_ALGN), -1);
  user_test_expect_eq("mask highest maskable signal",
    mask_signal(HIGHEST_MASKABLE_SIGNAL), 0);
  user_test_expect_eq("unmask highest maskable signal",
    unmask_signal(HIGHEST_MASKABLE_SIGNAL), 0);
  user_test_expect_eq("mask hello", mask_signal(SIGNAL_HELLO), 0);
  user_test_expect_eq("repeat mask hello", mask_signal(SIGNAL_HELLO), 0);
  user_test_expect_eq("unmask hello", unmask_signal(SIGNAL_HELLO), 0);
  user_test_expect_eq("repeat unmask hello", unmask_signal(SIGNAL_HELLO), 0);

  child_ready_sem = sem_open(0);
  child_continue_sem = sem_open(0);

  child = fork();
  if (child == 0){
    return masked_child();
  }
  sem_down(child_ready_sem);
  user_test_expect_eq("send first masked hello",
    signal_child(child, SIGNAL_HELLO), 0);
  user_test_expect_eq("send coalesced masked hello",
    signal_child(child, SIGNAL_HELLO), 0);
  sem_up(child_continue_sem);
  user_test_expect_eq("masked child exit status", wait_child(child),
    CHILD_READY_STATUS);

  child = fork();
  if (child == 0){
    user_test_expect_eq("register terminating hello handler",
      register_handler(SIGNAL_HELLO, (void*)async_exit_handler), 0);
    return waiting_child();
  }
  sem_down(child_ready_sem);

  // The child handler and this parent can run on different cores. User-space
  // printf emits one report through several writes, so reporting the send
  // result here could interleave with the handler's report. Preserve both
  // results, wait until the handler has printed and exited, and only then
  // emit the parent reports.
  int send_hello_rc = signal_child(child, SIGNAL_HELLO);
  int terminating_wait_rc = wait_child(child);
  user_test_expect_eq("send hello to terminating handler", send_hello_rc, 0);
  user_test_expect_eq("terminating handler exit status", terminating_wait_rc,
    ASYNC_EXIT_STATUS);

  // An unhandled, unmasked asynchronous signal uses the default termination
  // action and reports -1 to wait_child().
  child = fork();
  if (child == 0){
    return waiting_child();
  }
  sem_down(child_ready_sem);
  user_test_expect_eq("send unhandled terminate",
    signal_child(child, SIGNAL_TERMINATE), 0);
  user_test_expect_eq("unhandled terminate child status", wait_child(child),
    -1);

  // SIGNAL_KILL takes the same default termination action and cannot be
  // intercepted, regardless of the target's other registered handlers.
  child = fork();
  if (child == 0){
    return waiting_child();
  }
  sem_down(child_ready_sem);
  user_test_expect_eq("send SIGNAL_KILL", signal_child(child, SIGNAL_KILL), 0);
  user_test_expect_eq("SIGNAL_KILL child status", wait_child(child), -1);

  // Handler entry must carry the lower per-device IMR enables through rfe.
  // After the second semaphore handoff, sleep one PIT period so the child has
  // returned from sem_up() and entered its syscall-free loop before KILL is
  // sent. Termination then depends on a later PIT final-return hook.
  child = fork();
  if (child == 0){
    if (register_handler(SIGNAL_HELLO, (void*)spinning_handler) != 0){
      return -1;
    }
    return waiting_child();
  }
  sem_down(child_ready_sem);
  user_test_expect_eq("send hello to spinning handler",
    signal_child(child, SIGNAL_HELLO), 0);
  sem_down(child_ready_sem);
  sleep(1);
  user_test_expect_eq("send KILL to spinning handler",
    signal_child(child, SIGNAL_KILL), 0);
  user_test_expect_eq("PIT terminates syscall-free handler",
    wait_child(child), -1);

  child = fork();
  if (child == 0){
    user_test_expect_eq("register segmentation handler",
      register_handler(SIGNAL_SEG, (void*)seg_handler), 0);
    int* bad_ptr = (int*)SEG_FAULT_ADDRESS;
    *bad_ptr = 42;
    return -1;
  }
  user_test_expect_eq("segmentation handler exit status", wait_child(child),
    SEG_EXIT_STATUS);

  child = fork();
  if (child == 0){
    user_test_expect_eq("register invalid-instruction handler",
      register_handler(SIGNAL_ILL, (void*)ill_handler), 0);
    trigger_invalid_instruction();
    return -1;
  }
  user_test_expect_eq("invalid-instruction handler exit status",
    wait_child(child), ILL_EXIT_STATUS);

  child = fork();
  if (child == 0){
    if (register_handler(SIGNAL_ILL, (void*)ill_retry_handler) != 0){
      return -1;
    }
    trigger_invalid_instruction();
    return -1;
  }
  user_test_expect_eq("invalid-instruction sigreturn retries saved fault",
    wait_child(child), ILL_RETRY_EXIT_STATUS);

  child = fork();
  if (child == 0){
    user_test_expect_eq("register privileged-instruction handler",
      register_handler(SIGNAL_ILL, (void*)ill_handler), 0);
    trigger_privileged_instruction();
    return -1;
  }
  user_test_expect_eq("privileged-instruction handler exit status",
    wait_child(child), ILL_EXIT_STATUS);

  child = fork();
  if (child == 0){
    user_test_expect_eq("register misaligned-PC handler",
      register_handler(SIGNAL_ALGN, (void*)algn_handler), 0);
    trigger_misaligned_pc();
    return -1;
  }
  user_test_expect_eq("misaligned-PC handler exit status", wait_child(child),
    ALGN_EXIT_STATUS);

  child = fork();
  if (child == 0){
    trigger_invalid_instruction();
    return -1;
  }
  user_test_expect_eq("unhandled invalid-instruction status",
    wait_child(child), -1);

  child = fork();
  if (child == 0){
    trigger_privileged_instruction();
    return -1;
  }
  user_test_expect_eq("unhandled privileged-instruction status",
    wait_child(child), -1);

  child = fork();
  if (child == 0){
    trigger_misaligned_pc();
    return -1;
  }
  user_test_expect_eq("unhandled misaligned-PC status", wait_child(child), -1);

  child = fork();
  if (child == 0){
    user_test_expect_eq("register pre-exec hello handler",
      register_handler(SIGNAL_HELLO, (void*)async_exit_handler), 0);

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
  user_test_expect_eq("send hello after exec",
    signal_child(child, SIGNAL_HELLO), 0);
  user_test_expect_eq("exec cleared inherited handler", wait_child(child), -1);

  sem_close(child_ready_sem);
  sem_close(child_continue_sem);
  return 0;
}
