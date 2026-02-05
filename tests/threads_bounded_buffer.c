// Bounded buffer test.
// Purpose: verify bounded buffer blocks producers and delivers all items.

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

static void producer_thread(void* arg) {
  int producer_id = *(int*)arg;
  for (int i = 0; i < ITEMS_PER_PRODUCER; i++) {
    int id = producer_id * ITEMS_PER_PRODUCER + i;
    struct Item* item = malloc(sizeof(struct Item));
    assert(item != NULL, "bounded_buffer test: item allocation failed.\n");
    item->id = id;
    bounded_buffer_add(&buffer, &item->link);

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

void kernel_main(void) {
  say("***bounded_buffer test start\n", NULL);

  bounded_buffer_init(&buffer, BUFFER_CAPACITY);

  for (int i = 0; i < NUM_CONSUMERS; i++) {
    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "bounded_buffer test: Fun allocation failed.\n");
    fun->func = consumer_thread;
    fun->arg = NULL;
    thread(fun);
  }

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
