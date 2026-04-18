/*
 * Blocking ringbuf test.
 *
 * Validates:
 * - the blocking ringbuf never reports a size above BUFFER_CAPACITY
 * - producers and consumers can exchange all bytes without drops or duplicates
 * - blocking_ringbuf_remove_all() drains only currently available bytes and
 *   keeps the slot/data semaphores consistent for later reuse
 * - the fixed-size ring preserves FIFO order across wrap-around
 *
 * How:
 * - first exercise remove_all(), reuse, wrap-around, and destroy on a small
 *   local fixture while inspecting semaphore counts
 * - start NUM_PRODUCERS and NUM_CONSUMERS against one small shared ring
 * - producers publish disjoint byte ranges and check the observed size
 * - consumers drain bytes until they receive sentinels
 * - verify every byte in [0, TOTAL_BYTES) was seen exactly once
 */

#include "../kernel/blocking_ringbuf.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/machine.h"

#define NUM_PRODUCERS 2
#define NUM_CONSUMERS 2
#define BYTES_PER_PRODUCER 16
#define TOTAL_BYTES 32
#define BUFFER_CAPACITY 3
#define SENTINEL_BYTE 0xFF

static struct BlockingRingBuf ringbuf;
static int produced = 0;
static int consumed = 0;
static int consumers_done = 0;
static int seen[TOTAL_BYTES];

// Report an unsigned mismatch with the blocking_ringbuf-test prefix.
static void fail_uint(char* msg, unsigned got, unsigned expected){
  int args[2] = { (int)got, (int)expected };
  say("***blocking_ringbuf FAIL got=%u expected=%u\n", args);
  panic(msg);
}

static void expect_uint(unsigned got, unsigned expected, char* msg){
  if (got != expected){
    fail_uint(msg, got, expected);
  }
}

static void expect_byte(char got, unsigned expected, char* msg){
  if ((unsigned char)got != expected){
    fail_uint(msg, (unsigned char)got, expected);
  }
}

static void reset_workload_state(void){
  produced = 0;
  consumed = 0;
  consumers_done = 0;
  for (int i = 0; i < TOTAL_BYTES; i++){
    seen[i] = 0;
  }
}

// Check that remove_all() drains the same published bytes tracked by remove_sem
// and returns the freed slots to add_sem so the ring can be reused immediately.
static void test_remove_all_paths(void){
  struct BlockingRingBuf local_ringbuf;
  char drained[BUFFER_CAPACITY];

  blocking_ringbuf_init(&local_ringbuf, BUFFER_CAPACITY);
  expect_uint(blocking_ringbuf_size(&local_ringbuf), 0,
    "blocking_ringbuf test: init size mismatch\n");
  expect_uint(blocking_ringbuf_remove_all(&local_ringbuf, drained), 0,
    "blocking_ringbuf test: empty remove_all mismatch\n");
  expect_uint((unsigned)local_ringbuf.add_sem.count, BUFFER_CAPACITY,
    "blocking_ringbuf test: empty add_sem mismatch\n");
  expect_uint((unsigned)local_ringbuf.remove_sem.count, 0,
    "blocking_ringbuf test: empty remove_sem mismatch\n");

  blocking_ringbuf_add(&local_ringbuf, 1);
  blocking_ringbuf_add(&local_ringbuf, 2);
  blocking_ringbuf_add(&local_ringbuf, 3);
  expect_uint(blocking_ringbuf_size(&local_ringbuf), BUFFER_CAPACITY,
    "blocking_ringbuf test: size after add mismatch\n");
  expect_uint((unsigned)local_ringbuf.add_sem.count, 0,
    "blocking_ringbuf test: add_sem after add mismatch\n");
  expect_uint((unsigned)local_ringbuf.remove_sem.count, BUFFER_CAPACITY,
    "blocking_ringbuf test: remove_sem after add mismatch\n");

  expect_uint(blocking_ringbuf_remove_all(&local_ringbuf, drained),
    BUFFER_CAPACITY,
    "blocking_ringbuf test: remove_all count mismatch\n");
  expect_byte(drained[0], 1, "blocking_ringbuf test: remove_all first mismatch\n");
  expect_byte(drained[1], 2, "blocking_ringbuf test: remove_all second mismatch\n");
  expect_byte(drained[2], 3, "blocking_ringbuf test: remove_all third mismatch\n");
  expect_uint(blocking_ringbuf_size(&local_ringbuf), 0,
    "blocking_ringbuf test: size after remove_all mismatch\n");
  expect_uint((unsigned)local_ringbuf.add_sem.count, BUFFER_CAPACITY,
    "blocking_ringbuf test: add_sem after remove_all mismatch\n");
  expect_uint((unsigned)local_ringbuf.remove_sem.count, 0,
    "blocking_ringbuf test: remove_sem after remove_all mismatch\n");

  // Reuse immediately after remove_all(), then force wrap-around before
  // draining so FIFO order is checked across the ring boundary.
  blocking_ringbuf_add(&local_ringbuf, 4);
  blocking_ringbuf_add(&local_ringbuf, 5);
  expect_byte(blocking_ringbuf_remove(&local_ringbuf), 4,
    "blocking_ringbuf test: reuse remove mismatch\n");
  blocking_ringbuf_add(&local_ringbuf, 6);
  blocking_ringbuf_add(&local_ringbuf, 7);

  expect_uint(blocking_ringbuf_remove_all(&local_ringbuf, drained),
    BUFFER_CAPACITY,
    "blocking_ringbuf test: wrapped remove_all count mismatch\n");
  expect_byte(drained[0], 5,
    "blocking_ringbuf test: wrapped remove_all first mismatch\n");
  expect_byte(drained[1], 6,
    "blocking_ringbuf test: wrapped remove_all second mismatch\n");
  expect_byte(drained[2], 7,
    "blocking_ringbuf test: wrapped remove_all third mismatch\n");
  expect_uint((unsigned)local_ringbuf.add_sem.count, BUFFER_CAPACITY,
    "blocking_ringbuf test: add_sem after wrapped remove_all mismatch\n");
  expect_uint((unsigned)local_ringbuf.remove_sem.count, 0,
    "blocking_ringbuf test: remove_sem after wrapped remove_all mismatch\n");

  blocking_ringbuf_destroy(&local_ringbuf);
  expect_uint((unsigned)local_ringbuf.buf, 0,
    "blocking_ringbuf test: destroy did not clear buf pointer\n");
  expect_uint(local_ringbuf.capacity, 0,
    "blocking_ringbuf test: destroy did not clear capacity\n");
  expect_uint(local_ringbuf.head, 0,
    "blocking_ringbuf test: destroy did not clear head\n");
  expect_uint(local_ringbuf.tail, 0,
    "blocking_ringbuf test: destroy did not clear tail\n");
  expect_uint(blocking_ringbuf_size(&local_ringbuf), 0,
    "blocking_ringbuf test: destroy did not clear size\n");
}

// Publish one producer's fixed range of byte ids into the blocking ringbuf.
static void producer_thread(void* arg){
  int producer_id = *(int*)arg;

  for (int i = 0; i < BYTES_PER_PRODUCER; i++){
    unsigned byte = producer_id * BYTES_PER_PRODUCER + i;
    blocking_ringbuf_add(&ringbuf, (char)byte);

    // The ring should never grow past its configured capacity.
    unsigned size = blocking_ringbuf_size(&ringbuf);
    if (size > BUFFER_CAPACITY){
      int args[2] = { (int)size, BUFFER_CAPACITY };
      say("***blocking_ringbuf FAIL size=%d capacity=%d\n", args);
      panic("blocking_ringbuf test: size exceeded capacity\n");
    }

    __atomic_fetch_add(&produced, 1);
    yield();
  }
}

// Remove bytes until this consumer receives its sentinel.
static void consumer_thread(void* arg){
  (void)arg;

  while (true){
    unsigned byte = (unsigned char)blocking_ringbuf_remove(&ringbuf);
    if (byte == SENTINEL_BYTE){
      __atomic_fetch_add(&consumers_done, 1);
      return;
    }

    // Each real byte must be delivered exactly once.
    int old = __atomic_exchange_n(&seen[byte], 1);
    if (old != 0){
      int args[2] = { (int)byte, old };
      say("***blocking_ringbuf FAIL duplicate byte=%d old=%d\n", args);
      panic("blocking_ringbuf test: duplicate byte\n");
    }

    __atomic_fetch_add(&consumed, 1);
    yield();
  }
}

// Run the blocking-ringbuf workload and verify capacity plus delivery
// semantics.
void kernel_main(void){
  say("***blocking_ringbuf test start\n", NULL);

  test_remove_all_paths();

  blocking_ringbuf_init(&ringbuf, BUFFER_CAPACITY);
  reset_workload_state();

  // Start consumers before producers so the ring has to both fill and drain.
  for (int i = 0; i < NUM_CONSUMERS; i++){
    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "blocking_ringbuf test: Fun allocation failed.\n");
    fun->func = consumer_thread;
    fun->arg = NULL;
    thread(fun);
  }

  // Start producers that race to publish disjoint byte ranges.
  for (int i = 0; i < NUM_PRODUCERS; i++){
    int* id = malloc(sizeof(int));
    assert(id != NULL, "blocking_ringbuf test: id allocation failed.\n");
    *id = i;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "blocking_ringbuf test: Fun allocation failed.\n");
    fun->func = producer_thread;
    fun->arg = id;
    thread(fun);
  }

  while (__atomic_load_n(&produced) != TOTAL_BYTES){
    yield();
  }

  // One sentinel per consumer lets every worker exit cleanly.
  for (int i = 0; i < NUM_CONSUMERS; i++){
    blocking_ringbuf_add(&ringbuf, (char)SENTINEL_BYTE);
  }

  while (__atomic_load_n(&consumed) != TOTAL_BYTES){
    yield();
  }

  while (__atomic_load_n(&consumers_done) != NUM_CONSUMERS){
    yield();
  }

  // Every real byte id should have been observed exactly once.
  for (int i = 0; i < TOTAL_BYTES; i++){
    if (seen[i] != 1){
      int args[2] = { i, seen[i] };
      say("***blocking_ringbuf FAIL byte=%d seen=%d\n", args);
      panic("blocking_ringbuf test: missing byte\n");
    }
  }

  blocking_ringbuf_destroy(&ringbuf);

  say("***blocking_ringbuf ok\n", NULL);
  say("***blocking_ringbuf test complete\n", NULL);
}
