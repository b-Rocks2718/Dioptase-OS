/*
 * Bounded buffer test.
 *
 * Validates:
 * - the bounded buffer never reports a size above BUFFER_CAPACITY
 * - producers and consumers can exchange all items without drops or duplicates
 * - bounded_buffer_remove_all() drains only currently available items and keeps
 *   the slot/item semaphores consistent for later reuse
 *
 * How:
 * - first exercise bounded_buffer_remove_all() on a small local fixture and
 *   inspect both FIFO order and the semaphore counts before and after draining
 * - start NUM_PRODUCERS and NUM_CONSUMERS against one small shared buffer
 * - producers publish disjoint id ranges and check the observed size
 * - consumers drain items until they receive sentinels
 * - verify every id in [0, TOTAL_ITEMS) was seen exactly once
 */

#include "../kernel/bounded_buffer.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

#define NUM_PRODUCERS 2
#define NUM_CONSUMERS 2
#define ITEMS_PER_PRODUCER 10
#define TOTAL_ITEMS 20
#define BUFFER_CAPACITY 3
#define SENTINEL_ID (-1)

struct Item {
  struct GenericQueueElement link;
  int id;
};

static struct BoundedBuffer buffer;
static int produced = 0;
static int consumed = 0;
static int consumers_done = 0;
static int seen[TOTAL_ITEMS];

// Report an unsigned mismatch with the bounded_buffer-test prefix.
static void fail_uint(char* msg, unsigned got, unsigned expected) {
  int args[2] = { (int)got, (int)expected };
  say("***bounded_buffer FAIL got=%u expected=%u\n", args);
  panic(msg);
}

// Report a pointer mismatch with the bounded_buffer-test prefix.
static void fail_ptr(char* msg, void* got, void* expected) {
  int args[2] = { (int)got, (int)expected };
  say("***bounded_buffer FAIL got=0x%X expected=0x%X\n", args);
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

// Check that remove_all() drains the same published items tracked by remove_sem
// and returns the freed slots to add_sem so the buffer can be reused.
static void test_remove_all_paths(void) {
  struct BoundedBuffer local_buffer;
  struct Item a;
  struct Item b;
  struct Item c;
  struct Item d;

  bounded_buffer_init(&local_buffer, BUFFER_CAPACITY);
  expect_uint(bounded_buffer_size(&local_buffer), 0,
              "bounded_buffer test: init size mismatch\n");
  expect_ptr(bounded_buffer_remove_all(&local_buffer), NULL,
             "bounded_buffer test: empty remove_all mismatch\n");
  expect_uint((unsigned)local_buffer.add_sem.count, BUFFER_CAPACITY,
              "bounded_buffer test: empty add_sem mismatch\n");
  expect_uint((unsigned)local_buffer.remove_sem.count, 0,
              "bounded_buffer test: empty remove_sem mismatch\n");

  a.id = 1;
  b.id = 2;
  c.id = 3;
  d.id = 4;

  bounded_buffer_add(&local_buffer, &a.link);
  bounded_buffer_add(&local_buffer, &b.link);
  bounded_buffer_add(&local_buffer, &c.link);

  expect_uint(bounded_buffer_size(&local_buffer), BUFFER_CAPACITY,
              "bounded_buffer test: size after add mismatch\n");
  expect_uint((unsigned)local_buffer.add_sem.count, 0,
              "bounded_buffer test: add_sem after add mismatch\n");
  expect_uint((unsigned)local_buffer.remove_sem.count, BUFFER_CAPACITY,
              "bounded_buffer test: remove_sem after add mismatch\n");

  struct GenericQueueElement* head = bounded_buffer_remove_all(&local_buffer);
  struct GenericQueueElement* second = head != NULL ? head->next : NULL;
  struct GenericQueueElement* third = second != NULL ? second->next : NULL;

  expect_ptr(head, &a.link, "bounded_buffer test: remove_all head mismatch\n");
  expect_ptr(second, &b.link, "bounded_buffer test: remove_all second mismatch\n");
  expect_ptr(third, &c.link, "bounded_buffer test: remove_all third mismatch\n");
  expect_ptr(c.link.next, NULL,
             "bounded_buffer test: remove_all tail kept stale next pointer\n");

  expect_uint(bounded_buffer_size(&local_buffer), 0,
              "bounded_buffer test: size after remove_all mismatch\n");
  expect_uint((unsigned)local_buffer.add_sem.count, BUFFER_CAPACITY,
              "bounded_buffer test: add_sem after remove_all mismatch\n");
  expect_uint((unsigned)local_buffer.remove_sem.count, 0,
              "bounded_buffer test: remove_sem after remove_all mismatch\n");

  // Reuse must work immediately after remove_all() drains the buffer.
  bounded_buffer_add(&local_buffer, &d.link);
  expect_uint((unsigned)local_buffer.add_sem.count, BUFFER_CAPACITY - 1,
              "bounded_buffer test: add_sem after reuse add mismatch\n");
  expect_uint((unsigned)local_buffer.remove_sem.count, 1,
              "bounded_buffer test: remove_sem after reuse add mismatch\n");
  expect_ptr(bounded_buffer_remove(&local_buffer), &d.link,
             "bounded_buffer test: reuse remove mismatch\n");
  expect_uint((unsigned)local_buffer.add_sem.count, BUFFER_CAPACITY,
              "bounded_buffer test: add_sem after reuse remove mismatch\n");
  expect_uint((unsigned)local_buffer.remove_sem.count, 0,
              "bounded_buffer test: remove_sem after reuse remove mismatch\n");
}

// Publish one producer's fixed range of item ids into the bounded buffer.
static void producer_thread(void* arg) {
  int producer_id = *(int*)arg;
  for (int i = 0; i < ITEMS_PER_PRODUCER; i++) {
    int id = producer_id * ITEMS_PER_PRODUCER + i;
    struct Item* item = malloc(sizeof(struct Item));
    assert(item != NULL, "bounded_buffer test: item allocation failed.\n");
    item->id = id;
    bounded_buffer_add(&buffer, &item->link);

    // The buffer should never grow past its configured capacity.
    unsigned size = bounded_buffer_size(&buffer);
    if (size > BUFFER_CAPACITY) {
      int args[2] = { (int)size, BUFFER_CAPACITY };
      say("***bounded_buffer FAIL size=%d capacity=%d\n", args);
      panic("bounded_buffer test: size exceeded capacity\n");
    }

    __atomic_fetch_add(&produced, 1);
    yield();
  }
}

// Remove items until this consumer receives its sentinel.
static void consumer_thread(void* arg) {
  (void)arg;
  while (true) {
    struct GenericQueueElement* element = bounded_buffer_remove(&buffer);
    struct Item* item = (struct Item*)element;
    if (item->id == SENTINEL_ID) {
      free(item);
      __atomic_fetch_add(&consumers_done, 1);
      return;
    }

    // Each real item must be delivered exactly once.
    int old = __atomic_exchange_n(&seen[item->id], 1);
    if (old != 0) {
      int args[2] = { item->id, old };
      say("***bounded_buffer FAIL duplicate id=%d old=%d\n", args);
      panic("bounded_buffer test: duplicate item\n");
    }
    __atomic_fetch_add(&consumed, 1);
    free(item);
    yield();
  }
}

// Run the bounded-buffer workload and verify capacity plus delivery semantics.
void kernel_main(void) {
  say("***bounded_buffer test start\n", NULL);

  test_remove_all_paths();

  bounded_buffer_init(&buffer, BUFFER_CAPACITY);

  // Start consumers before producers so the queue can both fill and drain.
  for (int i = 0; i < NUM_CONSUMERS; i++) {
    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "bounded_buffer test: Fun allocation failed.\n");
    fun->func = consumer_thread;
    fun->arg = NULL;
    thread(fun);
  }

  // Start producers that fill the buffer from both sides of the race.
  for (int i = 0; i < NUM_PRODUCERS; i++) {
    int* id = malloc(sizeof(int));
    assert(id != NULL, "bounded_buffer test: id allocation failed.\n");
    *id = i;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "bounded_buffer test: Fun allocation failed.\n");
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
    assert(item != NULL, "bounded_buffer test: sentinel allocation failed.\n");
    item->id = SENTINEL_ID;
    bounded_buffer_add(&buffer, &item->link);
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
      say("***bounded_buffer FAIL id=%d seen=%d\n", args);
      panic("bounded_buffer test: missing item\n");
    }
  }

  say("***bounded_buffer ok\n", NULL);
  say("***bounded_buffer test complete\n", NULL);
}
