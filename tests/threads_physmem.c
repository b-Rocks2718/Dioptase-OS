/*
 * Physical page allocator test.
 *
 * Validates:
 * - physmem_alloc() returns only FRAME_SIZE-aligned addresses inside the
 *   documented physical-frame range
 * - concurrent workers never observe the same live frame at the same time
 * - repeated concurrent allocate/free churn does not lose or duplicate frames
 * - after concurrent churn, the allocator can still hand out the full frame
 *   pool exactly once and recover it on free
 *
 * How:
 * - start a set of worker threads together behind one shared go flag
 * - each worker keeps a small batch of frames live, stamps deterministic words
 *   into each page, yields to maximize overlap, then verifies and frees them
 * - a separate ownership table guarded by one test spin lock records which
 *   worker currently owns each frame index; any duplicate live allocation
 *   panics immediately
 * - after the threaded phase, exhaust the whole frame pool sequentially and
 *   prove that every frame index appears exactly once before freeing them all
 */

#include "../kernel/physmem.h"
#include "../kernel/threads.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/constants.h"
#include "../kernel/atomic.h"
#include "../kernel/semaphore.h"
#include "../kernel/heap.h"

#define NUM_WORKERS 8
#define LIVE_PAGES_PER_WORKER 4
#define CONCURRENCY_ROUNDS 16
#define NO_OWNER -1
#define TEST_FRAME_SIZE 4096 // Must match kernel FRAME_SIZE.
#define TEST_FRAME_WORDS 1024 // TEST_FRAME_SIZE / sizeof(unsigned)

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
static void* exhausted_pages[PHYS_FRAME_COUNT];

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
      void* page = physmem_alloc();
      note_live_page(page, tid);
      stamp_page(page, tid, round, slot);
      local_pages[slot] = page;

      if (((round + slot + tid) & 1) == 0) {
        yield();
      }
    }

    for (int slot = LIVE_PAGES_PER_WORKER - 1; slot >= 0; slot--) {
      void* page = local_pages[slot];
      check_page(page, tid, round, slot);
      note_freed_page(page, tid);
      physmem_free(page);

      if (((round + slot + tid) & 1) != 0) {
        yield();
      }
    }
  }

  sem_up(&finished_sem);
}

// Exhaust the documented frame pool and prove that every frame index appears
// exactly once. Any leak or duplicate introduced by the concurrent phase will
// show up here as an early panic or a repeated frame index.
static void exhaust_and_restore_pool(void) {
  for (int i = 0; i < PHYS_FRAME_COUNT; i++) {
    seen_once[i] = 0;
  }

  for (int i = 0; i < PHYS_FRAME_COUNT; i++) {
    void* page = physmem_alloc();
    int idx = frame_index(page);
    if (seen_once[idx] != 0) {
      int args[2] = { idx, (int)page };
      say("***physmem FAIL duplicate idx=%d page=0x%X\n", args);
      panic("physmem test: full-pool walk saw one frame twice\n");
    }
    seen_once[idx] = 1;
    exhausted_pages[i] = page;
  }

  for (int i = 0; i < PHYS_FRAME_COUNT; i++) {
    physmem_free(exhausted_pages[i]);
    exhausted_pages[i] = NULL;
  }
}

void kernel_main(void) {
  say("***physmem test start\n", NULL);

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

  exhaust_and_restore_pool();

  say("***physmem ok\n", NULL);
  say("***physmem test complete\n", NULL);
}
