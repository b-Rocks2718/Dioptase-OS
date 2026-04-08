/*
 * Concurrent anonymous private mmap()/munmap() stress test.
 *
 * Validates:
 * - each worker can hold multiple live private anonymous mappings at once
 * - freshly faulted anonymous pages start zeroed
 * - munmap() frees a middle hole that later mmap() calls can reuse exactly
 * - concurrent workers can churn mappings without corrupting each other's data
 *
 * How:
 * - start NUM_THREADS workers together so their faulting and unmapping overlaps
 * - in each round, every worker maps left/middle/right regions, faults them in,
 *   and records deterministic per-page contents
 * - the worker unmaps the middle region, remaps two single-page regions into
 *   that hole, verifies exact address reuse, and checks that the new pages are
 *   zero-filled before writing new contents
 * - yields between phases keep mappings live while other threads are doing the
 *   same work on other cores
 */

#include "../kernel/vmem.h"
#include "../kernel/physmem.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"

#define NUM_THREADS 8
#define ROUNDS 16

#define LEFT_PAGES 1
#define MIDDLE_PAGES 2
#define RIGHT_PAGES 1

struct ThreadArg {
  int id;
};

static int started = 0;
static int go = 0;
static int finished = 0;
static int completed_rounds = 0;

static unsigned words_per_page(void) {
  return FRAME_SIZE / sizeof(unsigned);
}

static unsigned sample_offset(unsigned sample) {
  unsigned words = words_per_page();
  switch (sample) {
    case 0: return 0;
    case 1: return 7;
    case 2: return words / 2;
    default: return words - 1;
  }
}

static unsigned pattern_word(int tid, int round, int region, unsigned index) {
  unsigned base = 0x51A70000u;
  base ^= ((unsigned)tid << 20);
  base ^= ((unsigned)round << 8);
  base ^= ((unsigned)region << 4);
  return base ^ index;
}

static void expect_ptr_eq(void* got, void* expected, int tid, int round, int step) {
  if (got != expected) {
    int args[5] = {tid, round, step, (int)got, (int)expected};
    say("***vmem private thread stress FAIL id=%d round=%d step=%d got=0x%X expect=0x%X\n", args);
    panic("vmem private thread stress: unexpected mmap address\n");
  }
}

static void expect_zeroed(unsigned* base, unsigned pages, int tid, int round, int region) {
  unsigned words = words_per_page();
  for (unsigned page = 0; page < pages; page++) {
    unsigned* page_base = base + page * words;
    for (unsigned sample = 0; sample < 4; sample++) {
      unsigned offset = sample_offset(sample);
      if (page_base[offset] != 0) {
        int args[6] = {tid, round, region, (int)page, (int)offset, (int)page_base[offset]};
        say("***vmem private thread stress FAIL id=%d round=%d region=%d page=%d word=%d got=0x%X\n", args);
        panic("vmem private thread stress: anonymous page was not zeroed\n");
      }
    }
  }
}

static void fill_region(unsigned* base, unsigned pages, int tid, int round, int region) {
  unsigned words = words_per_page();
  for (unsigned page = 0; page < pages; page++) {
    unsigned* page_base = base + page * words;
    for (unsigned sample = 0; sample < 4; sample++) {
      unsigned offset = sample_offset(sample);
      unsigned index = page * words + offset;
      page_base[offset] = pattern_word(tid, round, region, index);
    }
  }
}

static void check_region(unsigned* base, unsigned pages, int tid, int round, int region) {
  unsigned words = words_per_page();
  for (unsigned page = 0; page < pages; page++) {
    unsigned* page_base = base + page * words;
    for (unsigned sample = 0; sample < 4; sample++) {
      unsigned offset = sample_offset(sample);
      unsigned index = page * words + offset;
      unsigned expected = pattern_word(tid, round, region, index);
      if (page_base[offset] != expected) {
        int args[7] = {tid, round, region, (int)page, (int)offset, (int)page_base[offset], (int)expected};
        say("***vmem private thread stress FAIL id=%d round=%d region=%d page=%d word=%d got=0x%X expect=0x%X\n", args);
        panic("vmem private thread stress: mapping contents corrupted\n");
      }
    }
  }
}

static void vmem_worker(void* arg) {
  struct ThreadArg* a = (struct ThreadArg*)arg;
  int tid = a->id;

  __atomic_fetch_add(&started, 1);
  while (__atomic_load_n(&go) == 0) {
    yield();
  }

  for (int round = 0; round < ROUNDS; round++) {
    unsigned* left = (unsigned*)mmap(LEFT_PAGES * FRAME_SIZE, false, NULL, 0);
    unsigned* middle = (unsigned*)mmap(MIDDLE_PAGES * FRAME_SIZE, false, NULL, 0);
    unsigned* right = (unsigned*)mmap(RIGHT_PAGES * FRAME_SIZE, false, NULL, 0);

    expect_ptr_eq(middle, (void*)((unsigned)left + LEFT_PAGES * FRAME_SIZE), tid, round, 0);
    expect_ptr_eq(right, (void*)((unsigned)middle + MIDDLE_PAGES * FRAME_SIZE), tid, round, 1);

    expect_zeroed(left, LEFT_PAGES, tid, round, 0);
    fill_region(left, LEFT_PAGES, tid, round, 0);
    expect_zeroed(middle, MIDDLE_PAGES, tid, round, 1);
    fill_region(middle, MIDDLE_PAGES, tid, round, 1);
    expect_zeroed(right, RIGHT_PAGES, tid, round, 2);
    fill_region(right, RIGHT_PAGES, tid, round, 2);

    yield();

    check_region(left, LEFT_PAGES, tid, round, 0);
    check_region(middle, MIDDLE_PAGES, tid, round, 1);
    check_region(right, RIGHT_PAGES, tid, round, 2);

    munmap(middle);

    yield();

    unsigned* refill0 = (unsigned*)mmap(FRAME_SIZE, false, NULL, 0);
    unsigned* refill1 = (unsigned*)mmap(FRAME_SIZE, false, NULL, 0);

    expect_ptr_eq(refill0, middle, tid, round, 2);
    expect_ptr_eq(refill1, (void*)((unsigned)middle + FRAME_SIZE), tid, round, 3);

    expect_zeroed(refill0, 1, tid, round, 3);
    fill_region(refill0, 1, tid, round, 3);
    expect_zeroed(refill1, 1, tid, round, 4);
    fill_region(refill1, 1, tid, round, 4);

    yield();

    check_region(left, LEFT_PAGES, tid, round, 0);
    check_region(right, RIGHT_PAGES, tid, round, 2);
    check_region(refill0, 1, tid, round, 3);
    check_region(refill1, 1, tid, round, 4);

    munmap(refill1);
    munmap(right);
    munmap(refill0);
    munmap(left);

    __atomic_fetch_add(&completed_rounds, 1);

    if ((round & 1) == 0) {
      yield();
    }
  }

  __atomic_fetch_add(&finished, 1);
}

void kernel_main(void) {
  say("***vmem private thread stress test start\n", NULL);

  for (int i = 0; i < NUM_THREADS; i++) {
    struct ThreadArg* arg = malloc(sizeof(struct ThreadArg));
    assert(arg != NULL, "vmem private thread stress: ThreadArg allocation failed.\n");
    arg->id = i;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "vmem private thread stress: Fun allocation failed.\n");
    fun->func = vmem_worker;
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

  int expected_rounds = NUM_THREADS * ROUNDS;
  int actual_rounds = __atomic_load_n(&completed_rounds);
  if (actual_rounds != expected_rounds) {
    int args[2] = {actual_rounds, expected_rounds};
    say("***vmem private thread stress FAIL rounds=%d expected=%d\n", args);
    panic("vmem private thread stress: completed round count mismatch\n");
  }

  int args[2] = {NUM_THREADS, ROUNDS};
  say("***vmem private thread stress ok threads=%d rounds=%d\n", args);
  say("***vmem private thread stress test complete\n", NULL);
}
