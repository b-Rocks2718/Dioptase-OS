// Blocking queue test.
// Purpose: verify blocking removal and FIFO delivery under contention.

#include "../kernel/blocking_queue.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

#define NUM_PRODUCERS 2
#define NUM_CONSUMERS 3
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

static void consumer_thread(void* arg) {
  (void)arg;
  while (true) {
    struct GenericQueueElement* element = blocking_queue_remove(&queue);
    struct Item* item = (struct Item*)element;
    if (item->id == SENTINEL_ID) {
      free(item);
      __atomic_fetch_add(&consumers_done, 1);
      return;
    }

    int old = __atomic_exchange_n(&seen[item->id], 1);
    if (old != 0) {
      int args[2] = { item->id, old };
      say("***blocking_queue FAIL duplicate id=%d old=%d\n", args);
      panic("blocking_queue test: duplicate item\n");
    }
    __atomic_fetch_add(&consumed, 1);
    free(item);
  }
}

void kernel_main(void) {
  say("***blocking_queue test start\n", NULL);

  blocking_queue_init(&queue);

  for (int i = 0; i < NUM_CONSUMERS; i++) {
    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "blocking_queue test: Fun allocation failed.\n");
    fun->func = consumer_thread;
    fun->arg = NULL;
    thread(fun);
  }

  for (int i = 0; i < PRE_PRODUCE_YIELDS; i++) {
    yield();
  }

  if (__atomic_load_n(&consumed) != 0) {
    int args[2] = { __atomic_load_n(&consumed), 0 };
    say("***blocking_queue FAIL consumed=%d expected=%d\n", args);
    panic("blocking_queue test: consumers ran without producers\n");
  }

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
