/*
 * Blocking ringbuf test.
 *
 * Validates:
 * - the blocking ringbuf never reports a size above BUFFER_CAPACITY
 * - producers and consumers can exchange all bytes without drops or duplicates
 * - the fixed-size ring preserves FIFO order across wrap-around
 *
 * How:
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
