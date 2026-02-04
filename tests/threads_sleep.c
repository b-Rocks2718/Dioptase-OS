// Sleep test.
// Purpose: verify that sleep blocks a thread for at least the requested
// number of jiffies and eventually resumes it.

#include "../kernel/threads.h"
#include "../kernel/pit.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

#define SLEEP_JIFFIES 8
#define MAX_WAIT_JIFFIES 300

static int done = 0;
static unsigned observed_sleep = 0;

// Purpose: sleep and report the observed jiffy delta.
// Inputs: arg is unused.
// Preconditions: PIT initialized; kernel mode.
// Postconditions: done is set and observed_sleep contains elapsed jiffies.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static void sleeper_thread(void* arg) {
  (void)arg;

  unsigned start = current_jiffies;
  sleep(SLEEP_JIFFIES);
  unsigned end = current_jiffies;

  observed_sleep = end - start;
  __atomic_store_n(&done, 1);
}

static void sleep_1(void* arg) {
  sleep(25);
  say("*** sleep_1 ok\n", NULL);
}

static void sleep_2(void* arg) {
  sleep(50);
  say("*** sleep_2 ok\n", NULL);
}

static void sleep_3(void* arg) {
  sleep(75);
  say("*** sleep_3 ok\n", NULL);
}

// Purpose: validate basic sleep semantics.
// Inputs: none.
// Outputs: prints pass/fail status; panics on failure.
// Preconditions: kernel mode; scheduler initialized; PIT running.
// Postconditions: sleeper thread resumes after at least SLEEP_JIFFIES ticks.
// CPU state assumptions: kernel mode; interrupts enabled except where noted.
void kernel_main(void) {
  say("***sleep test start\n", NULL);

  struct Fun* fun = malloc(sizeof(struct Fun));
  assert(fun != NULL, "sleep test: Fun allocation failed.\n");
  fun->func = sleeper_thread;
  fun->arg = NULL;
  thread(fun);

  unsigned wait_start = current_jiffies;
  while (__atomic_load_n(&done) == 0) {
    unsigned elapsed = current_jiffies - wait_start;
    if (elapsed > MAX_WAIT_JIFFIES) {
      int args[2] = { (int)elapsed, MAX_WAIT_JIFFIES };
      say("***sleep FAIL timeout elapsed=%d max=%d\n", args);
      panic("sleep test: sleeper did not resume\n");
    }
    yield();
  }

  if (observed_sleep < SLEEP_JIFFIES) {
    int args[2] = { (int)observed_sleep, SLEEP_JIFFIES };
    say("***sleep FAIL slept=%d expected_min=%d\n", args);
    panic("sleep test: resumed too early\n");
  }

  struct Fun* sleep_1_fun = malloc(sizeof(struct Fun));
  struct Fun* sleep_2_fun = malloc(sizeof(struct Fun));
  struct Fun* sleep_3_fun = malloc(sizeof(struct Fun));

  sleep_1_fun->func = sleep_1;
  sleep_1_fun->arg = NULL;
  sleep_2_fun->func = sleep_2;
  sleep_2_fun->arg = NULL;
  sleep_3_fun->func = sleep_3;
  sleep_3_fun->arg = NULL;

  thread(sleep_3_fun);
  thread(sleep_1_fun);
  thread(sleep_2_fun);

  sleep(100);

  say("***sleep test complete\n", NULL);
}
