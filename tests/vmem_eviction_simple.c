/*
 * Concurrent shared file-backed eviction / TLB shootdown test.
 *
 * Validates:
 * - multiple cores can map the same shared file page
 * - page_evict() removes the live mapping for every thread holding that page
 * - a stale TLB entry does not survive eviction and let a core keep reading
 *   the old physical page
 * - after eviction, a reread of the same virtual address refaults from the
 *   backing file and sees the file's new contents
 *
 * How:
 * - all threads map the same file-backed shared page and confirm an initial
 *   baseline string
 * - the main thread locks the underlying page, calls page_evict(), then
 *   rewrites the backing file with a different string
 * - every thread rereads the same virtual address; if TLB shootdown is wrong,
 *   a core can keep seeing the old page contents instead of faulting back in
 *   the rewritten file bytes
 */

#include "../kernel/vmem.h"
#include "../kernel/ext.h"
#include "../kernel/threads.h"
#include "../kernel/barrier.h"
#include "../kernel/heap.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"
#include "../kernel/physmem.h"
#include "../kernel/machine.h"

#define WORKER_COUNT   3
#define TEST_FILE_NAME "evict.txt"
#define TEST_BYTES     16

#define OLD_TEXT "OLD-COHERENCY-1!"
#define NEW_TEXT "NEW-COHERENCY-2!"

static struct Barrier phase_barrier;
static int finished = 0;

struct WorkerArg {
  int id;
};

static void expect_bytes(char* got, char* expected, int thread_id, int phase) {
  for (unsigned i = 0; i < TEST_BYTES; ++i) {
    if (got[i] != expected[i]) {
      int args[4] = {thread_id, phase, (int)i, ((int)got[i] << 8) | (unsigned char)expected[i]};
      say("***vmem evict tlb FAIL id=%d phase=%d offset=%d pair=0x%X\n", args);
      panic("vmem evict tlb: mapping contents mismatch\n");
    }
  }
}

static struct Page* mapped_page(char* mapping) {
  unsigned* pte = vmem_get_pte(get_pid(), (unsigned)mapping, false);
  assert(pte != NULL, "vmem evict tlb: failed to look up PTE\n");
  assert(*pte & VMEM_VALID, "vmem evict tlb: expected mapping to be valid\n");
  return get_page(pte_phys_addr(*pte));
}

static char* map_fixture_page(void) {
  struct Node* file = node_find(&fs.root, TEST_FILE_NAME);
  assert(file != NULL, "vmem evict tlb: failed to open fixture file\n");

  char* mapping = mmap(TEST_BYTES, file, 0, MMAP_READ | MMAP_WRITE | MMAP_SHARED);
  assert(mapping != NULL, "vmem evict tlb: mmap returned NULL\n");
  node_free(file);
  return mapping;
}

static void shared_evict_worker(void* arg) {
  struct WorkerArg* worker = (struct WorkerArg*)arg;
  int id = worker->id;
  char* mapping = map_fixture_page();

  expect_bytes(mapping, (char*)OLD_TEXT, id, 0);
  // Phase 0: all threads have faulted in the original shared page.
  barrier_sync(&phase_barrier);

  // Phase 1: wait for kernel_main() to evict the page and rewrite the file.
  barrier_sync(&phase_barrier);
  expect_bytes(mapping, (char*)NEW_TEXT, id, 1);
  yield();
  expect_bytes(mapping, (char*)NEW_TEXT, id, 2);

  // Phase 2: all threads have observed the refaulted page contents.
  barrier_sync(&phase_barrier);
  munmap(mapping);

  __atomic_fetch_add(&finished, 1);
}

void kernel_main(void) {
  say("***vmem eviction simple test start\n", NULL);

  barrier_init(&phase_barrier, WORKER_COUNT + 1);
  for (int i = 0; i < WORKER_COUNT; ++i) {
    struct WorkerArg* arg = malloc(sizeof(struct WorkerArg));
    assert(arg != NULL, "vmem evict tlb: failed to allocate worker args\n");
    arg->id = i;

    struct Fun* fun = malloc(sizeof(struct Fun));
    assert(fun != NULL, "vmem evict tlb: failed to allocate thread metadata\n");
    fun->func = shared_evict_worker;
    fun->arg = arg;
    thread(fun);
  }

  char* mapping = map_fixture_page();
  expect_bytes(mapping, (char*)OLD_TEXT, -1, 0);

  // Phase 0: workers have faulted in the original page and are paused.
  barrier_sync(&phase_barrier);

  struct Page* page = mapped_page(mapping);
  physmem_page_lock(page);
  page_evict(page); // Definitely not pinned because writers have already faulted in the page

  struct Node* file = node_find(&fs.root, TEST_FILE_NAME);
  assert(file != NULL, "vmem evict tlb: failed to reopen fixture file\n");
  unsigned wrote = node_write_all(file, 0, TEST_BYTES, (char*)NEW_TEXT);
  assert(wrote == TEST_BYTES, "vmem evict tlb: failed to rewrite file bytes\n");
  node_free(file);
  expect_bytes(mapping, (char*)NEW_TEXT, -1, 1);

  // Phase 1: release workers only after eviction and rewrite have completed.
  barrier_sync(&phase_barrier);

  // Phase 2: all threads have observed the refaulted page contents.
  barrier_sync(&phase_barrier);
  munmap(mapping);

  while (__atomic_load_n(&finished) != WORKER_COUNT) {
    yield();
  }

  barrier_destroy(&phase_barrier);

  say("***vmem eviction simple test complete\n", NULL);
}