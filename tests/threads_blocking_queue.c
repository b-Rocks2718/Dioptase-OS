/*
 * Blocking queue test.
 *
 * Validates:
 * - blocking_queue_try_remove reports empty/full state without desynchronizing
 *   the queue from its semaphore-backed availability count
 * - blocking_queue_remove_all drains only currently available items and leaves
 *   the blocking remove path consistent afterward
 * - blocking_queue_remove blocks consumers until producers add work
 * - blocking_queue_try_remove can run concurrently with blocking removers and
 *   still deliver every produced item exactly once
 *
 * How:
 * - first exercise the non-blocking helpers on a small local queue fixture and
 *   inspect both FIFO order and semaphore count after each step
 * - then start a mixed consumer set: some threads block in remove(), others
 *   poll with try_remove() and yield when the queue is empty
 * - start producers that publish disjoint id ranges into the shared queue
 * - enqueue one sentinel per consumer after the real items are produced
 * - verify every id in [0, TOTAL_ITEMS) was seen exactly once
 */

#include "../kernel/blocking_queue.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

#define NUM_PRODUCERS 2
#define NUM_BLOCKING_CONSUMERS 2
#define NUM_TRY_CONSUMERS 2
#define NUM_CONSUMERS 4
#define ITEMS_PER_PRODUCER 8
#define TOTAL_ITEMS 16
#define SENTINEL_ID (-1)
#define PRE_PRODUCE_YIELDS 32

struct Item {
  struct GenericQueueElement link;
  int id;
};

static struct BlockingQueue queue;
static int produced = 0;
static int consumed = 0;
static int consumers_done = 0;
static int seen[TOTAL_ITEMS];

// Report an unsigned mismatch with the blocking_queue-test prefix.
static void fail_uint(char* msg, unsigned got, unsigned expected) {
  int args[2] = { (int)got, (int)expected };
  say("***blocking_queue FAIL got=%u expected=%u\n", args);
  panic(msg);
}

// Report a pointer mismatch with the blocking_queue-test prefix.
static void fail_ptr(char* msg, void* got, void* expected) {
  int args[2] = { (int)got, (int)expected };
  say("***blocking_queue FAIL got=0x%X expected=0x%X\n", args);
  panic(msg);
}

static void expect_uint(unsigned got, unsigned expected, char* msg) {
  if (got != expected) {
    fail_uint(msg, got, expected);
  }
}

static void expect_ptr(void* got, void* expected, char* msg) {
  if (got != expected) {
    fail_ptr(msg, got, expected);
  }
}

static void reset_workload_state(void) {
  produced = 0;
  consumed = 0;
  consumers_done = 0;
  for (int i = 0; i < TOTAL_ITEMS; i++) {
    seen[i] = 0;
  }
}

// Check that blocking_queue_try_remove() and remove_all() consume the same
// availability permits as the blocking remove path.
static void test_nonblocking_paths(void) {
  struct BlockingQueue local_queue;
  struct Item a;
  struct Item b;
  struct Item c;

  blocking_queue_init(&local_queue);
  expect_uint(blocking_queue_size(&local_queue), 0,
              "blocking_queue test: init size mismatch\n");
  expect_ptr(blocking_queue_try_remove(&local_queue), NULL,
             "blocking_queue test: empty try_remove mismatch\n");
  expect_uint((unsigned)local_queue.sem.count, 0,
              "blocking_queue test: empty semaphore count mismatch\n");

  a.id = 1;
  b.id = 2;
  c.id = 3;

  blocking_queue_add(&local_queue, &a.link);
  blocking_queue_add(&local_queue, &b.link);
  expect_uint(blocking_queue_size(&local_queue), 2,
              "blocking_queue test: size after add mismatch\n");
  expect_uint((unsigned)local_queue.sem.count, 2,
              "blocking_queue test: semaphore count after add mismatch\n");

  expect_ptr(blocking_queue_try_remove(&local_queue), &a.link,
             "blocking_queue test: try_remove FIFO mismatch\n");
  expect_uint(blocking_queue_size(&local_queue), 1,
              "blocking_queue test: size after try_remove mismatch\n");
  expect_uint((unsigned)local_queue.sem.count, 1,
              "blocking_queue test: semaphore count after try_remove mismatch\n");

  expect_ptr(blocking_queue_remove(&local_queue), &b.link,
             "blocking_queue test: blocking remove mismatch\n");
  expect_uint(blocking_queue_size(&local_queue), 0,
              "blocking_queue test: size after blocking remove mismatch\n");
  expect_uint((unsigned)local_queue.sem.count, 0,
              "blocking_queue test: semaphore count after blocking remove mismatch\n");

  blocking_queue_add(&local_queue, &a.link);
  blocking_queue_add(&local_queue, &b.link);
  blocking_queue_add(&local_queue, &c.link);

  struct GenericQueueElement* head = blocking_queue_remove_all(&local_queue);
  struct GenericQueueElement* second = head != NULL ? head->next : NULL;
  struct GenericQueueElement* third = second != NULL ? second->next : NULL;
  expect_ptr(head, &a.link, "blocking_queue test: remove_all head mismatch\n");
  expect_ptr(second, &b.link, "blocking_queue test: remove_all second mismatch\n");
  expect_ptr(third, &c.link, "blocking_queue test: remove_all third mismatch\n");
  expect_ptr(c.link.next, NULL,
             "blocking_queue test: remove_all tail kept stale next pointer\n");
  expect_uint(blocking_queue_size(&local_queue), 0,
              "blocking_queue test: size after remove_all mismatch\n");
  expect_uint((unsigned)local_queue.sem.count, 0,
              "blocking_queue test: semaphore count after remove_all mismatch\n");
  expect_ptr(blocking_queue_try_remove(&local_queue), NULL,
             "blocking_queue test: try_remove should see empty queue after remove_all\n");
}

// Publish one producer's fixed range of item ids into the queue.
static void producer_thread(void* arg) {
  int producer_id = *(int*)arg;
  for (int i = 0; i < ITEMS_PER_PRODUCER; i++) {
    int id = producer_id * ITEMS_PER_PRODUCER + i;
    struct Item* item = malloc(sizeof(struct Item));
    assert(item != NULL, "blocking_queue test: item allocation failed.\n");
    item->id = id;
    blocking_queue_add(&queue, &item->link);
    __atomic_fetch_add(&produced, 1);
    yield();
  }
}

// Record one delivered item, ensuring each payload is observed exactly once.
static void handle_consumed_item(struct Item* item) {
  // Each real item must be delivered exactly once.
  int old = __atomic_exchange_n(&seen[item->id], 1);
  if (old != 0) {
    int args[2] = { item->id, old };
    say("***blocking_queue FAIL duplicate id=%d old=%d\n", args);
    panic("blocking_queue test: duplicate item\n");
  }
  __atomic_fetch_add(&consumed, 1);
  free(item);
}

// Remove items with the blocking path until this consumer receives its sentinel.
static void blocking_consumer_thread(void* arg) {
  (void)arg;
  while (true) {
    struct GenericQueueElement* element = blocking_queue_remove(&queue);
    struct Item* item = (struct Item*)element;
    if (item->id == SENTINEL_ID) {
      free(item);
      __atomic_fetch_add(&consumers_done, 1);
      return;
    }

    handle_consumed_item(item);
  }
}

// Poll with try_remove() so the non-blocking path races against producers and
// blocking removers on the same queue.
static void try_consumer_thread(void* arg) {
  (void)arg;
  while (true) {
    struct GenericQueueElement* element = blocking_queue_try_remove(&queue);
    if (element == NULL) {
      yield();
      continue;
    }

    struct Item* item = (struct Item*)element;
    if (item->id == SENTINEL_ID) {
      free(item);
      __atomic_fetch_add(&consumers_done, 1);
      return;
    }

    handle_consumed_item(item);
  }
}

// Run the producer/consumer workload and verify nothing is lost or duplicated.
void kernel_main(void) {
  say("***blocking_queue test start\n", NULL);

  test_nonblocking_paths();

  blocking_queue_init(&queue);
  reset_workload_state();

  // Start blocking consumers first so remove() has to park on the empty queue.
  for (int i = 0; i < NUM_BLOCKING_CONSUMERS; i++) {
    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "blocking_queue test: Fun allocation failed.\n");
    fun->func = blocking_consumer_thread;
    fun->arg = NULL;
    thread(fun);
  }

  // Start polling consumers too so try_remove() races against producers and
  // blocking removers once work appears.
  for (int i = 0; i < NUM_TRY_CONSUMERS; i++) {
    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "blocking_queue test: Fun allocation failed.\n");
    fun->func = try_consumer_thread;
    fun->arg = NULL;
    thread(fun);
  }

  // Give consumers time to park on the empty queue.
  for (int i = 0; i < PRE_PRODUCE_YIELDS; i++) {
    yield();
  }

  if (__atomic_load_n(&consumed) != 0) {
    int args[2] = { __atomic_load_n(&consumed), 0 };
    say("***blocking_queue FAIL consumed=%d expected=%d\n", args);
    panic("blocking_queue test: consumers ran without producers\n");
  }

  // Now start the producers that fill the queue.
  for (int i = 0; i < NUM_PRODUCERS; i++) {
    int* id = malloc(sizeof(int));
    assert(id != NULL, "blocking_queue test: id allocation failed.\n");
    *id = i;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "blocking_queue test: Fun allocation failed.\n");
    fun->func = producer_thread;
    fun->arg = id;
    thread(fun);
  }

  while (__atomic_load_n(&produced) != TOTAL_ITEMS) {
    yield();
  }

  // One sentinel per consumer lets every worker exit cleanly.
  for (int i = 0; i < NUM_CONSUMERS; i++) {
    struct Item* item = malloc(sizeof(struct Item));
    assert(item != NULL, "blocking_queue test: sentinel allocation failed.\n");
    item->id = SENTINEL_ID;
    blocking_queue_add(&queue, &item->link);
  }

  while (__atomic_load_n(&consumed) != TOTAL_ITEMS) {
    yield();
  }

  while (__atomic_load_n(&consumers_done) != NUM_CONSUMERS) {
    yield();
  }

  // Every real item id should have been observed exactly once.
  for (int i = 0; i < TOTAL_ITEMS; i++) {
    if (seen[i] != 1) {
      int args[2] = { i, seen[i] };
      say("***blocking_queue FAIL id=%d seen=%d\n", args);
      panic("blocking_queue test: missing item\n");
    }
  }

  say("***blocking_queue ok\n", NULL);
  say("***blocking_queue test complete\n", NULL);
}
