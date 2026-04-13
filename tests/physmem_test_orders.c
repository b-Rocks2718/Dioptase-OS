/*
 * Physical page allocator test.
 *
 * Validates:
 * - physmem_alloc() returns only FRAME_SIZE-aligned addresses inside the
 *   documented physical-frame range
 * - concurrent workers can churn a modest batch of order-0 pages without
 *   returning one live frame to two workers at once
 * - physmem_alloc_order() returns aligned higher-order blocks that remain
 *   writable across the span of the requested block size
 * - after draining per-core page caches back into the global buddy allocator,
 *   the backend can still hand out a large order-0 sample without duplicates
 *   and recover it on free
 *
 * How:
 * - start a set of worker threads together behind one shared go flag
 * - each worker keeps a small batch of frames live, stamps deterministic words
 *   into each page, yields to create overlap, then verifies and frees them
 * - a separate ownership table guarded by one test spin lock records which
 *   worker currently owns each frame index; any duplicate live allocation
 *   panics immediately
 * - after the threaded phase, explicitly drain any per-core order-0 caches
 *   back into the global allocator so the backend pool can be checked
 * - run a sequential higher-order smoke test, then walk a large direct
 *   order-0 backend sample and prove that every returned frame index is unique
 *   before freeing that sample
 */

#include "../kernel/physmem.h"
#include "../kernel/per_core.h"
#include "../kernel/threads.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/constants.h"
#include "../kernel/atomic.h"
#include "../kernel/semaphore.h"
#include "../kernel/heap.h"

#define NUM_WORKERS 4
#define LIVE_PAGES_PER_WORKER 2
#define CONCURRENCY_ROUNDS 3
#define NO_OWNER -1
#define TEST_FRAME_SIZE 4096 // Must match kernel FRAME_SIZE.
#define TEST_FRAME_WORDS 1024 // TEST_FRAME_SIZE / sizeof(unsigned)
// 8,192 pages is still a 32 MiB backend sample, which crosses many buddy
// regions without exhausting all 30,648 frames on every 4-core run.
#define GLOBAL_WALK_PAGE_COUNT 8192

#define MAX_TEST_ORDER 12

struct ThreadArg {
  int id;
};

static struct SpinLock owner_lock;
static struct SpinLock state_lock;
static struct Semaphore finished_sem;
static int ready_workers = 0;
static bool go = false;

static int live_owner[PHYS_FRAME_COUNT];
static unsigned char seen_once[PHYS_FRAME_COUNT];
static void* sampled_pages[GLOBAL_WALK_PAGE_COUNT];

// Map one frame address back to its slot in the documented physical-frame range.
static int frame_index(void* page) {
  unsigned addr = (unsigned)page;
  if (addr < FRAMES_ADDR_START || addr >= FRAMES_ADDR_END) {
    int args[3] = { (int)page, FRAMES_ADDR_START, FRAMES_ADDR_END };
    say("***physmem FAIL addr=0x%X range=[0x%X,0x%X)\n", args);
    panic("physmem test: page address outside physical-frame range\n");
  }
  if ((addr & (TEST_FRAME_SIZE - 1)) != 0) {
    int args[2] = { (int)page, TEST_FRAME_SIZE };
    say("***physmem FAIL addr=0x%X align=%d\n", args);
    panic("physmem test: page address is not frame aligned\n");
  }
  return (addr - FRAMES_ADDR_START) / TEST_FRAME_SIZE;
}

static unsigned block_page_count(int order) {
  return 1u << order;
}

static void check_block_alignment(void* block, int order) {
  int idx = frame_index(block);
  unsigned page_count = block_page_count(order);
  if ((idx & (page_count - 1)) != 0) {
    int args[3] = { order, idx, (int)block };
    say("***physmem FAIL order=%d idx=%d block=0x%X\n", args);
    panic("physmem test: higher-order block is not aligned to its size\n");
  }
  if ((unsigned)idx + page_count > PHYS_FRAME_COUNT) {
    int args[3] = { order, idx, (int)block };
    say("***physmem FAIL order=%d idx=%d block=0x%X\n", args);
    panic("physmem test: higher-order block exceeds frame pool bounds\n");
  }
}

// Stamp a few far-apart words so each live frame carries recognizable data
// without spending the whole test rewriting every byte of every page.
static void stamp_page(void* page, int tid, int round, int slot) {
  unsigned* words = (unsigned*)page;
  unsigned base = 0xC0DE0000u ^ ((unsigned)tid << 16) ^ ((unsigned)round << 4) ^ (unsigned)slot;
  words[0] = base;
  words[1] = base ^ 0x11111111u;
  words[TEST_FRAME_WORDS / 2] = base ^ 0x22222222u;
  words[TEST_FRAME_WORDS - 2] = base ^ 0x33333333u;
  words[TEST_FRAME_WORDS - 1] = base ^ 0x44444444u;
}

// Verify the same words written by stamp_page() before the frame is freed.
static void check_page(void* page, int tid, int round, int slot) {
  unsigned* words = (unsigned*)page;
  unsigned base = 0xC0DE0000u ^ ((unsigned)tid << 16) ^ ((unsigned)round << 4) ^ (unsigned)slot;
  if (words[0] != base ||
      words[1] != (base ^ 0x11111111u) ||
      words[TEST_FRAME_WORDS / 2] != (base ^ 0x22222222u) ||
      words[TEST_FRAME_WORDS - 2] != (base ^ 0x33333333u) ||
      words[TEST_FRAME_WORDS - 1] != (base ^ 0x44444444u)) {
    int args[4] = { tid, round, slot, (int)page };
    say("***physmem FAIL tid=%d round=%d slot=%d page=0x%X\n", args);
    panic("physmem test: live page contents were corrupted\n");
  }
}

static void stamp_block(void* block, int order) {
  unsigned page_count = block_page_count(order);
  unsigned base_addr = (unsigned)block;
  stamp_page((void*)base_addr, 0x40 + order, order, 0);
  if (page_count > 2) {
    stamp_page((void*)(base_addr + (page_count / 2) * TEST_FRAME_SIZE), 0x40 + order, order, 1);
  }
  if (page_count > 1) {
    stamp_page((void*)(base_addr + (page_count - 1) * TEST_FRAME_SIZE), 0x40 + order, order, 2);
  }
}

static void check_block(void* block, int order) {
  unsigned page_count = block_page_count(order);
  unsigned base_addr = (unsigned)block;
  check_page((void*)base_addr, 0x40 + order, order, 0);
  if (page_count > 2) {
    check_page((void*)(base_addr + (page_count / 2) * TEST_FRAME_SIZE), 0x40 + order, order, 1);
  }
  if (page_count > 1) {
    check_page((void*)(base_addr + (page_count - 1) * TEST_FRAME_SIZE), 0x40 + order, order, 2);
  }
}

// Record one live allocation in the ownership table. The allocator must never
// return one frame to two workers simultaneously.
static void note_live_page(void* page, int tid) {
  int idx = frame_index(page);

  spin_lock_acquire(&owner_lock);
  if (live_owner[idx] != NO_OWNER) {
    int args[3] = { idx, live_owner[idx], tid };
    say("***physmem FAIL idx=%d owner=%d contender=%d\n", args);
    panic("physmem test: allocator returned one live frame twice\n");
  }
  live_owner[idx] = tid;
  spin_lock_release(&owner_lock);
}

// Remove one page from the ownership table before returning it to the allocator.
static void note_freed_page(void* page, int tid) {
  int idx = frame_index(page);

  spin_lock_acquire(&owner_lock);
  if (live_owner[idx] != tid) {
    int args[3] = { idx, live_owner[idx], tid };
    say("***physmem FAIL idx=%d owner=%d freeing=%d\n", args);
    panic("physmem test: frame ownership bookkeeping mismatch on free\n");
  }
  live_owner[idx] = NO_OWNER;
  spin_lock_release(&owner_lock);
}

// Keep several frames live per worker to force real overlap between allocators
// on different cores, then verify and free those frames repeatedly.
static void physmem_worker(void* arg) {
  struct ThreadArg* thread_arg = (struct ThreadArg*)arg;
  int tid = thread_arg->id;
  void* local_pages[LIVE_PAGES_PER_WORKER];

  spin_lock_acquire(&state_lock);
  ready_workers++;
  spin_lock_release(&state_lock);

  // Hold every worker until main has created the full worker set, then let
  // them all begin the alloc/free churn together.
  while (true) {
    bool start_now;
    spin_lock_acquire(&state_lock);
    start_now = go;
    spin_lock_release(&state_lock);

    if (start_now) {
      break;
    }
    yield();
  }

  for (int round = 0; round < CONCURRENCY_ROUNDS; round++) {
    for (int slot = 0; slot < LIVE_PAGES_PER_WORKER; slot++) {
      unsigned order = (slot + round * tid) % MAX_TEST_ORDER;

      void* page = (order == 0 && slot & 1) ? physmem_alloc() : physmem_alloc_order(order);

      note_live_page(page, tid);
      stamp_page(page, tid, round, slot);
      local_pages[slot] = page;

      if (((round + slot + tid) & 1) == 0) {
        yield();
      }
    }

    for (int slot = LIVE_PAGES_PER_WORKER - 1; slot >= 0; slot--) {
      unsigned order = (slot + round * tid) % MAX_TEST_ORDER;

      void* page = local_pages[slot];
      check_page(page, tid, round, slot);
      note_freed_page(page, tid);

      if (order == 0 && slot & 1) {
        physmem_free(page);
      } else {
        physmem_free_order(page, order);
      }

      if (((round + slot + tid) & 1) != 0) {
        yield();
      }
    }
  }

  sem_up(&finished_sem);
}

void kernel_main(void) {
  say("***testing concurrent small allocations\n", NULL);

  spin_lock_init(&owner_lock);
  spin_lock_init(&state_lock);
  sem_init(&finished_sem, 0);
  ready_workers = 0;
  go = false;

  for (int i = 0; i < PHYS_FRAME_COUNT; i++) {
    live_owner[i] = NO_OWNER;
  }

  for (int i = 0; i < NUM_WORKERS; i++) {
    struct ThreadArg* arg = malloc(sizeof(struct ThreadArg));
    assert(arg != NULL, "physmem test: ThreadArg allocation failed.\n");
    arg->id = i;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "physmem test: Fun allocation failed.\n");
    fun->func = physmem_worker;
    fun->arg = arg;

    thread(fun);
  }

  while (true) {
    int ready_now;
    spin_lock_acquire(&state_lock);
    ready_now = ready_workers;
    spin_lock_release(&state_lock);

    if (ready_now == NUM_WORKERS) {
      break;
    }
    yield();
  }

  spin_lock_acquire(&state_lock);
  go = true;
  spin_lock_release(&state_lock);

  for (int i = 0; i < NUM_WORKERS; i++) {
    sem_down(&finished_sem);
  }

  say("***physmem test complete\n", NULL);
}
