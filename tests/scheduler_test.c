/*
 * Scheduler policy test.
 *
 * Validates:
 * - local ready-queue selection prefers higher-priority queues in the configured
 *   weighted 4:2:1 pattern when all priority classes are runnable
 * - MLFQ selection removes the highest non-empty level first within one
 *   priority class
 * - priority class selection wins over MLFQ level, so a lower-level
 *   high-priority thread still runs before a top-level normal-priority thread
 * - MLFQ boost promotes queued level-1/2 threads back to level 0 and refreshes
 *   their level-0 quantum
 *
 * How:
 * - save the current core's local ready queues, replace them with small TCB
 *   fixtures, and drive local_queue_remove() with controlled scheduler_iters
 * - verify the exact removal order for weighted priority slots and per-priority
 *   MLFQ level ordering
 * - call the scheduler's boost helper on queued fixtures and check their
 *   updated level and quantum
 */

#include "../kernel/scheduler.h"
#include "../kernel/per_core.h"
#include "../kernel/queue.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/interrupts.h"
#include "../kernel/threads.h"

#define PRIORITY_CYCLE_SLOTS 7
#define NORMAL_PRIORITY_SLOT 4

extern void mlfq_boost(void);

struct SavedReadyQueues {
  struct TCB* heads[PRIORITY_LEVELS][MLFQ_LEVELS];
  unsigned scheduler_iters;
  bool preemption_was;
};

// Report a pointer mismatch with the scheduler-test prefix.
static void fail_ptr(char* msg, void* got, void* expected) {
  int args[2] = { (int)got, (int)expected };
  say("***scheduler FAIL got=0x%X expected=0x%X\n", args);
  panic(msg);
}

// Report an unsigned mismatch with the scheduler-test prefix.
static void fail_uint(char* msg, unsigned got, unsigned expected) {
  int args[2] = { (int)got, (int)expected };
  say("***scheduler FAIL got=%u expected=%u\n", args);
  panic(msg);
}

// Expect one TCB pointer result.
static void expect_tcb(struct TCB* got, struct TCB* expected, char* msg) {
  if (got != expected) {
    fail_ptr(msg, got, expected);
  }
}

// Expect one unsigned result.
static void expect_uint(unsigned got, unsigned expected, char* msg) {
  if (got != expected) {
    fail_uint(msg, got, expected);
  }
}

// Initialize one ready-queue fixture TCB with the requested priority and level.
static void init_ready_tcb(struct TCB* tcb,
                           enum ThreadPriority priority,
                           enum MLFQ_LEVEL level,
                           int remaining_quantum,
                           struct TCB* stale_next) {
  tcb->priority = priority;
  tcb->mlfq_level = level;
  tcb->remaining_quantum = remaining_quantum;
  tcb->core_affinity = ANY_CORE;
  tcb->can_preempt = true;
  tcb->next = stale_next;
}

// Drain this core's local ready queues so the test can install isolated fixtures.
static void save_ready_queues(struct SavedReadyQueues* saved) {
  saved->preemption_was = preemption_disable();
  int was = interrupts_disable();
  struct PerCore* core = get_per_core();
  saved->scheduler_iters = core->scheduler_iters;
  for (int priority = LOW_PRIORITY; priority <= HIGH_PRIORITY; priority++) {
    for (int level = LEVEL_ZERO; level <= LEVEL_TWO; level++) {
      saved->heads[priority][level] = queue_remove_all(&core->ready_queue[priority][level]);
    }
  }
  interrupts_restore(was);
}

// Restore the saved local ready queues after one isolated scheduler subtest.
static void restore_ready_queues(struct SavedReadyQueues* saved) {
  int was = interrupts_disable();
  struct PerCore* core = get_per_core();
  core->scheduler_iters = saved->scheduler_iters;
  for (int priority = LOW_PRIORITY; priority <= HIGH_PRIORITY; priority++) {
    for (int level = LEVEL_ZERO; level <= LEVEL_TWO; level++) {
      // Drop any stack-backed test fixtures before restoring the original queue.
      queue_remove_all(&core->ready_queue[priority][level]);
      struct TCB* node = saved->heads[priority][level];
      while (node != NULL) {
        struct TCB* next = node->next;
        node->next = NULL;
        queue_add(&core->ready_queue[priority][level], node);
        node = next;
      }
    }
  }
  interrupts_restore(was);
  preemption_restore(saved->preemption_was);
}

// Override scheduler_iters for the next local_queue_remove() call.
static void set_scheduler_slot(unsigned slot) {
  int was = interrupts_disable();
  get_per_core()->scheduler_iters = slot;
  interrupts_restore(was);
}

// Check that MLFQ removal prefers level 0, then level 1, then level 2.
static void test_mlfq_level_order(void) {
  struct SavedReadyQueues saved;
  struct TCB stale;
  struct TCB level_two;
  struct TCB level_zero;
  struct TCB level_one;

  save_ready_queues(&saved);

  init_ready_tcb(&stale, NORMAL_PRIORITY, LEVEL_ZERO, 0, NULL);
  init_ready_tcb(&level_two, NORMAL_PRIORITY, LEVEL_TWO, 17, &stale);
  init_ready_tcb(&level_zero, NORMAL_PRIORITY, LEVEL_ZERO, 19, &stale);
  init_ready_tcb(&level_one, NORMAL_PRIORITY, LEVEL_ONE, 23, &stale);

  local_queue_add(&level_two);
  local_queue_add(&level_zero);
  local_queue_add(&level_one);

  set_scheduler_slot(NORMAL_PRIORITY_SLOT);
  expect_tcb(local_queue_remove(), &level_zero,
             "scheduler test: MLFQ level-0 remove mismatch\n");
  set_scheduler_slot(NORMAL_PRIORITY_SLOT);
  expect_tcb(local_queue_remove(), &level_one,
             "scheduler test: MLFQ level-1 remove mismatch\n");
  set_scheduler_slot(NORMAL_PRIORITY_SLOT);
  expect_tcb(local_queue_remove(), &level_two,
             "scheduler test: MLFQ level-2 remove mismatch\n");
  set_scheduler_slot(NORMAL_PRIORITY_SLOT);
  expect_tcb(local_queue_remove(), NULL,
             "scheduler test: MLFQ queue should be empty after removes\n");

  restore_ready_queues(&saved);
}

// Check that priority selection still wins before MLFQ level selection.
static void test_priority_beats_level(void) {
  struct SavedReadyQueues saved;
  struct TCB stale;
  struct TCB high_level_two;
  struct TCB normal_level_zero;

  save_ready_queues(&saved);

  init_ready_tcb(&stale, NORMAL_PRIORITY, LEVEL_ZERO, 0, NULL);
  init_ready_tcb(&high_level_two, HIGH_PRIORITY, LEVEL_TWO, 29, &stale);
  init_ready_tcb(&normal_level_zero, NORMAL_PRIORITY, LEVEL_ZERO, 31, &stale);

  local_queue_add(&high_level_two);
  local_queue_add(&normal_level_zero);

  set_scheduler_slot(0);
  expect_tcb(local_queue_remove(), &high_level_two,
             "scheduler test: high priority should beat normal level-0 thread\n");
  set_scheduler_slot(NORMAL_PRIORITY_SLOT);
  expect_tcb(local_queue_remove(), &normal_level_zero,
             "scheduler test: normal thread remove mismatch after high priority\n");

  restore_ready_queues(&saved);
}

// Check the configured 4:2:1 priority slot pattern when all queues stay non-empty.
static void test_priority_weight_pattern(void) {
  struct SavedReadyQueues saved;
  struct TCB stale;
  struct TCB high;
  struct TCB normal;
  struct TCB low;
  struct TCB* expected[PRIORITY_CYCLE_SLOTS];

  save_ready_queues(&saved);

  init_ready_tcb(&stale, NORMAL_PRIORITY, LEVEL_ZERO, 0, NULL);
  init_ready_tcb(&high, HIGH_PRIORITY, LEVEL_ZERO, 37, &stale);
  init_ready_tcb(&normal, NORMAL_PRIORITY, LEVEL_ZERO, 41, &stale);
  init_ready_tcb(&low, LOW_PRIORITY, LEVEL_ZERO, 43, &stale);

  local_queue_add(&high);
  local_queue_add(&normal);
  local_queue_add(&low);

  expected[0] = &high;
  expected[1] = &high;
  expected[2] = &high;
  expected[3] = &high;
  expected[4] = &normal;
  expected[5] = &normal;
  expected[6] = &low;

  for (unsigned slot = 0; slot < PRIORITY_CYCLE_SLOTS; slot++) {
    set_scheduler_slot(slot);
    struct TCB* got = local_queue_remove();
    expect_tcb(got, expected[slot],
               "scheduler test: priority slot remove mismatch\n");
    local_queue_add(got);
  }

  restore_ready_queues(&saved);
}

// Check that boost promotes queued level-1/2 threads and refreshes level-0 quantum.
static void test_mlfq_boost_promotes_queued_threads(void) {
  struct SavedReadyQueues saved;
  struct TCB stale;
  struct TCB level_one;
  struct TCB level_two;

  save_ready_queues(&saved);

  init_ready_tcb(&stale, NORMAL_PRIORITY, LEVEL_ZERO, 0, NULL);
  init_ready_tcb(&level_one, NORMAL_PRIORITY, LEVEL_ONE, 3, &stale);
  init_ready_tcb(&level_two, NORMAL_PRIORITY, LEVEL_TWO, 7, &stale);

  local_queue_add(&level_one);
  local_queue_add(&level_two);

  mlfq_boost();

  expect_uint(level_one.mlfq_level, LEVEL_ZERO,
              "scheduler test: boost did not promote level-one thread\n");
  expect_uint(level_two.mlfq_level, LEVEL_ZERO,
              "scheduler test: boost did not promote level-two thread\n");
  expect_uint(level_one.remaining_quantum, TIME_QUANTUM[LEVEL_ZERO],
              "scheduler test: boost did not refresh level-one quantum\n");
  expect_uint(level_two.remaining_quantum, TIME_QUANTUM[LEVEL_ZERO],
              "scheduler test: boost did not refresh level-two quantum\n");

  set_scheduler_slot(NORMAL_PRIORITY_SLOT);
  expect_tcb(local_queue_remove(), &level_one,
             "scheduler test: boosted level-one FIFO remove mismatch\n");
  set_scheduler_slot(NORMAL_PRIORITY_SLOT);
  expect_tcb(local_queue_remove(), &level_two,
             "scheduler test: boosted level-two FIFO remove mismatch\n");

  restore_ready_queues(&saved);
}

void kernel_main(void) {
  say("***scheduler test start\n", NULL);

  test_mlfq_level_order();
  say("***scheduler mlfq levels ok\n", NULL);

  test_priority_beats_level();
  say("***scheduler priority over level ok\n", NULL);

  test_priority_weight_pattern();
  say("***scheduler priority weights ok\n", NULL);

  test_mlfq_boost_promotes_queued_threads();
  say("***scheduler boost ok\n", NULL);

  say("***scheduler test complete\n", NULL);
}
