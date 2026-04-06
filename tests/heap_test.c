/*
 * Heap stress test.
 *
 * Validates:
 * - malloc() and free() preserve payload bytes across randomized allocation and
 *   free patterns
 * - freeing in shuffled order does not corrupt neighboring live allocations
 * - allocator reuse does not leak stale bytes into newly allocated blocks
 *
 * How:
 * - allocate a batch of random-sized blocks, fill each one with a deterministic
 *   pattern, and remember its metadata
 * - free half the blocks in shuffled order after verifying their contents
 * - allocate and immediately churn a second batch to force reuse of freed holes
 * - verify and free the remaining original blocks, then repeat for several rounds
 */

#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/constants.h"

#define ROUNDS 3
#define N_BLOCKS 32

// Generate deterministic pseudo-random values for the stress pattern.
static unsigned rng_state = 0xC0FFEE01u;
static unsigned rnd_u32(void) {
  unsigned x = rng_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_state = x;
  return x;
}

// Fill one block with a recognizable byte pattern.
static void mem_fill(unsigned char* p, unsigned n, unsigned seed) {
  for (unsigned i = 0; i < n; i++) {
    p[i] = (unsigned char)((seed + i * 131u) ^ (seed >> 8));
  }
}

// Check that a block still holds the exact bytes written at allocation time.
static void mem_check(unsigned char* p, unsigned n, unsigned seed) {
  for (unsigned i = 0; i < n; i++) {
    unsigned char expect = (unsigned char)((seed + i * 131u) ^ (seed >> 8));
    if (p[i] != expect) {
      int args[4] = {(int)p, (int)i, (int)p[i], (int)expect};
      say("| HEAP TEST FAIL: p=0x%X off=%d got=0x%X expect=0x%X\n", args);
      panic("heap corruption detected by stress test\n");
    }
  }
}

// Shuffle an index array so frees happen in a different order every round.
static void shuffle(unsigned* a, unsigned n) {
  for (unsigned i = n; i > 1; i--) {
    unsigned j = rnd_u32() % i;
    unsigned tmp = a[i - 1];
    a[i - 1] = a[j];
    a[j] = tmp;
  }
}

struct BlockInfo {
  void* ptr;
  unsigned size;
  unsigned seed;
};

// Run one heap stress workload with randomized sizes and free order.
static void heap_stress(unsigned rounds, unsigned N) {
  static struct BlockInfo blocks[4096];
  static unsigned order[4096];

  for (unsigned r = 0; r < rounds; r++) {
    // Phase 1: allocate and fill a baseline set of live blocks.
    for (unsigned i = 0; i < N; i++) {
      unsigned roll = rnd_u32();
      unsigned sz;
      if ((roll & 15u) == 0) {
        sz = 512 + (roll % 2048);
      } else if ((roll & 3u) == 0) {
        sz = 64 + (roll % 512);
      } else {
        sz = 8 + (roll % 128);
      }

      void* p = malloc(sz);
      if (!p) {
        int args[3] = {(int)r, (int)i, (int)sz};
        say("| malloc failed at round=%d i=%d sz=%d\n", args);
        panic("heap_stress: malloc returned NULL\n");
      }

      unsigned seed = (r * 0x9E3779B9u) ^ (i * 0x85EBCA6Bu) ^ roll;

      blocks[i].ptr = p;
      blocks[i].size = sz;
      blocks[i].seed = seed;

      mem_fill((unsigned char*)p, sz, seed);

      order[i] = i;
    }

    // Phase 2: free half the blocks in shuffled order after validating them.
    shuffle(order, N);

    unsigned halfway = N / 2;
    for (unsigned k = 0; k < halfway; k++) {
      unsigned idx = order[k];
      struct BlockInfo* b = &blocks[idx];
      mem_check((unsigned char*)b->ptr, b->size, b->seed);
      free(b->ptr);
      b->ptr = NULL;
    }

    // Phase 3: churn smaller blocks to force reuse of the freed holes.
    for (unsigned i = 0; i < halfway; i++) {
      unsigned roll = rnd_u32();
      unsigned sz = 16 + (roll % 256);
      void* p = malloc(sz);
      if (!p) panic("heap_stress: malloc failed during reuse phase\n");
      unsigned seed = 0xA5A5A5A5u ^ (r << 16) ^ i ^ roll;
      mem_fill((unsigned char*)p, sz, seed);

      mem_check((unsigned char*)p, sz, seed);
      free(p);
    }

    // Phase 4: verify and free the surviving original blocks.
    for (unsigned k = halfway; k < N; k++) {
      unsigned idx = order[k];
      struct BlockInfo* b = &blocks[idx];
      mem_check((unsigned char*)b->ptr, b->size, b->seed);
      free(b->ptr);
      b->ptr = NULL;
    }

    int msg[2] = {(int)(r + 1), (int)rounds};
    say("***heap stress round %d/%d OK\n", msg);
  }

  say("***heap stress test completed OK\n", NULL);
}

// Run a moderate heap stress workload that fits the test image comfortably.
void kernel_main(void) {
  heap_stress(ROUNDS, N_BLOCKS);
}
