// Thread-safe heap stress test.
// Purpose: ensure heap malloc/free are safe under concurrent access.

#include "../kernel/heap.h"
#include "../kernel/threads.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/constants.h"
#include "../kernel/machine.h"

#define NUM_THREADS 6
#define ROUNDS 3
#define N_BLOCKS 64

struct ThreadArg {
  int id;
};

struct BlockInfo {
  void* ptr;
  unsigned size;
  unsigned seed;
};

static int started = 0;
static int go = 0;
static int finished = 0;

static struct BlockInfo blocks[NUM_THREADS][N_BLOCKS];
static unsigned order[NUM_THREADS][N_BLOCKS];
static unsigned rng_state[NUM_THREADS];

// Purpose: per-thread deterministic RNG.
// Inputs: tid identifies the thread-local state.
// Outputs: pseudo-random u32.
// Preconditions: tid in [0, NUM_THREADS).
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static unsigned rnd_u32(int tid) {
  unsigned x = rng_state[tid];
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_state[tid] = x;
  return x;
}

static void mem_fill(unsigned char* p, unsigned n, unsigned seed) {
  for (unsigned i = 0; i < n; i++) {
    p[i] = (unsigned char)((seed + i * 131u) ^ (seed >> 8));
  }
}

static void mem_check(unsigned char* p, unsigned n, unsigned seed) {
  for (unsigned i = 0; i < n; i++) {
    unsigned char expect = (unsigned char)((seed + i * 131u) ^ (seed >> 8));
    if (p[i] != expect) {
      int args[4] = {(int)p, (int)i, (int)p[i], (int)expect};
      say("| HEAP THREAD FAIL: p=0x%X off=%d got=0x%X expect=0x%X\n", args);
      panic("heap thread safety corruption detected\n");
    }
  }
}

static void shuffle(int tid) {
  for (unsigned i = N_BLOCKS; i > 1; i--) {
    unsigned j = rnd_u32(tid) % i;
    unsigned tmp = order[tid][i - 1];
    order[tid][i - 1] = order[tid][j];
    order[tid][j] = tmp;
  }
}

// Purpose: perform per-thread heap stress cycles.
// Inputs: arg points to ThreadArg.
// Preconditions: heap initialized; arg non-NULL.
// Postconditions: per-thread blocks allocated and freed without corruption.
// CPU state assumptions: kernel mode; interrupts may be enabled or disabled.
static void heap_worker(void* arg) {
  struct ThreadArg* a = (struct ThreadArg*)arg;
  int tid = a->id;

  __atomic_fetch_add(&started, 1);
  while (__atomic_load_n(&go) == 0) {
    yield();
  }

  for (unsigned r = 0; r < ROUNDS; r++) {
    for (unsigned i = 0; i < N_BLOCKS; i++) {
      unsigned roll = rnd_u32(tid);
      unsigned sz;
      if ((roll & 15u) == 0) {
        sz = 256 + (roll % 512);
      } else if ((roll & 3u) == 0) {
        sz = 64 + (roll % 256);
      } else {
        sz = 8 + (roll % 128);
      }

      void* p = malloc(sz);
      if (!p) {
        int args[3] = {(int)r, (int)i, (int)sz};
        say("| heap thread malloc failed r=%d i=%d sz=%d\n", args);
        panic("heap_threadsafe: malloc returned NULL\n");
      }

      unsigned seed = (r * 0x9E3779B9u) ^ (i * 0x85EBCA6Bu) ^ roll ^ (tid << 16);

      blocks[tid][i].ptr = p;
      blocks[tid][i].size = sz;
      blocks[tid][i].seed = seed;
      mem_fill((unsigned char*)p, sz, seed);
      order[tid][i] = i;
    }

    shuffle(tid);

    unsigned halfway = N_BLOCKS / 2;
    for (unsigned k = 0; k < halfway; k++) {
      unsigned idx = order[tid][k];
      struct BlockInfo* b = &blocks[tid][idx];
      mem_check((unsigned char*)b->ptr, b->size, b->seed);
      free(b->ptr);
      b->ptr = NULL;
    }

    for (unsigned i = 0; i < halfway; i++) {
      unsigned roll = rnd_u32(tid);
      unsigned sz = 16 + (roll % 128);
      void* p = malloc(sz);
      if (!p) panic("heap_threadsafe: malloc failed during reuse phase\n");
      unsigned seed = 0xA5A5A5A5u ^ (r << 16) ^ i ^ roll ^ (tid << 24);
      mem_fill((unsigned char*)p, sz, seed);
      mem_check((unsigned char*)p, sz, seed);
      free(p);
    }

    for (unsigned k = halfway; k < N_BLOCKS; k++) {
      unsigned idx = order[tid][k];
      struct BlockInfo* b = &blocks[tid][idx];
      mem_check((unsigned char*)b->ptr, b->size, b->seed);
      free(b->ptr);
      b->ptr = NULL;
    }
  }

  __atomic_fetch_add(&finished, 1);
}

void kernel_main(void) {
  say("***heap threadsafe test start\n", NULL);

  for (int i = 0; i < NUM_THREADS; i++) {
    rng_state[i] = 0xC0FFEE01u ^ (unsigned)(i * 0x9E3779B9u);
  }

  for (int i = 0; i < NUM_THREADS; i++) {
    struct ThreadArg* arg = malloc(sizeof(struct ThreadArg));
    assert(arg != NULL, "heap threadsafe: ThreadArg allocation failed.\n");
    arg->id = i;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "heap threadsafe: Fun allocation failed.\n");
    fun->func = heap_worker;
    fun->arg = arg;

    thread(fun);
  }

  while (__atomic_load_n(&started) != NUM_THREADS) {
    yield();
  }

  __atomic_store_n(&go, 1);

  while (__atomic_load_n(&finished) != NUM_THREADS) {
    yield();
  }

  say("***heap threadsafe ok\n", NULL);
  say("***heap threadsafe test complete\n", NULL);
}
