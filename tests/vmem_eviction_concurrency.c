/*
 * Eviction concurrency stress test.
 *
 * 3 writer threads repeatedly write disjoint bytes into a shared, file-backed
 * mapping while one evictor thread continuously evicts the page. After a
 * fixed number of rounds we quiesce and verify the backing file contains the
 * writers' last committed values.
 * 
 * Checks that faulting and evicting at the same time still produces correct behavior;
 * no deadlocks and no incoherence
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
#include "../kernel/page_cache.h"
#include "../kernel/barrier.h"

#define WRITERS        3
#define ROUNDS         64
#define TEST_FILE_NAME "evict_conc.txt"
#define TEST_BYTES     512

static int finished = 0;
static int progress = 0;

struct Barrier barrier;

struct WriterArg {
  int id;
};

// Copied over from page cache
static struct PageCacheEntry* page_cache_lookup(struct PageCache* cache, struct Node* node, unsigned offset) {
  unsigned hash = ((unsigned)(node->cached) ^ offset) % cache->hash_map_size;
  struct PageCacheEntry* entry = cache->hash_map[hash];
  // iterate linked list until we find a match
  while (entry) {
    if (entry->key.inode == node->cached && entry->key.offset == offset) {
      return entry;
    }
    entry = entry->next;
  }
  return NULL;
}

static void expect_file_contents(char* file_bytes) {
  for (int row = 0; row < WRITERS; ++row) {
    for (int i = 0; i < ROUNDS - 1; ++i) {
      char expected = 'a' + (i % 26);
      char got = file_bytes[(row * ROUNDS) + i];
      if (got != expected) {
        int args[4] = {row, i, (int)got, (int)expected};
        say("***vmem eviction concurrency FAIL row=%d offset=%d got=0x%X expected=0x%X\n", args);
      }
    }

    char got = file_bytes[(row * ROUNDS) + (ROUNDS - 1)];
    if (got != '\n') {
      int args[3] = {row, ROUNDS - 1, (int)got};
      say("***vmem eviction concurrency FAIL row=%d offset=%d got=0x%X expected=0xA\n", args);
      panic("vmem eviction concurrency: missing row terminator\n");
    }
  }
}

static void writer_thread(void* arg) {
  struct WriterArg* a = (struct WriterArg*)arg;
  int id = a->id;
  struct Node* file = node_find(&fs.root, TEST_FILE_NAME);
  assert(file != NULL, "evict conc: failed to open fixture file\n");
  assert(file->cached != NULL, "NULL CACHED INODE\n");
  char* mapping = mmap(TEST_BYTES, file, 0, MMAP_READ | MMAP_WRITE | MMAP_SHARED);
  node_free(file);
  assert(mapping != NULL, "evict conc: mmap returned NULL\n");

  barrier_sync(&barrier);

  for (int r = 0; r < ROUNDS - 1; ++r) {
    // write one byte slot owned by this writer each round
    unsigned offset = (id * ROUNDS) + r; // disjoint low-order bytes
    mapping[offset] = 97 + (r % 26);

    __atomic_fetch_add(&progress, 1);

    // widen the race window; give eviction a chance to run
    if ((r & 3) == 0)
      yield();
  }
  mapping[(id * ROUNDS) + (ROUNDS - 1)] = '\n';
  __atomic_fetch_add(&finished, 1);

  int args[1] = {id};
  say("writer %d finished\n", args);
}

static void evictor_thread(void* _arg) {
  // Evictor runs on one core and continuously finds the cached page and evicts
  struct Node* file = node_find(&fs.root, TEST_FILE_NAME);
  assert(file != NULL, "evict conc: failed to open fixture file\n");

  while (finished < WRITERS) {
    blocking_lock_acquire(&page_cache.lock);
    // acquire page cache entry to find the page frame (if present)
    struct PageCacheEntry* entry = page_cache_lookup(&page_cache, file, 0); // NOTE this is only safe because we're the only thing that can evict
    blocking_lock_release(&page_cache.lock);
    if (entry) {
      say("z\n", NULL);
      struct Page* page = get_page(entry->page_data, "get page - concurrency test");
      physmem_page_lock(page);
      if (!(page->flags & PG_PINNED)) {
        page_evict(page);
      } else {
        physmem_page_unlock(page);
      }
    } else {
      yield();
    }
    yield();
  }

  __atomic_fetch_add(&finished, 1);
  node_free(file);
  say("evictor finished\n", NULL);
}

void kernel_main(void) {
  say("***vmem eviction concurrency test start\n", NULL);

  struct Node* file = node_make_file(&fs.root, TEST_FILE_NAME);
  assert(file != NULL, "evict conc: failed to create fixture file\n");

  // spawn writers
  barrier_init(&barrier, WRITERS);
  for (int i = 0; i < WRITERS; ++i) {
    struct WriterArg* a = malloc(sizeof(*a));
    a->id = i;
    struct Fun* worker = malloc(sizeof(*worker));
    worker->func = writer_thread;
    worker->arg = a;
    thread(worker);
  }

  // spawn evictor
  struct Fun* evictor = malloc(sizeof(*evictor));
  evictor->func = evictor_thread;
  evictor->arg = NULL;
  thread(evictor);

  // wait for writers to finish
  while (__atomic_load_n(&finished) != WRITERS + 1) {
    sleep(50);
    int args[2] = {progress, WRITERS * ROUNDS - WRITERS};
    say("progress: %d / %d\n", args);
  }

  // verify backing file contains last writer values
  char* file_bytes = mmap(TEST_BYTES, file, 0, MMAP_READ);
  assert(file_bytes != NULL, "evict conc: mmap for verification returned NULL\n");
  expect_file_contents(file_bytes);

  node_free(file);
  say("***vmem eviction concurrency test complete\n", NULL);
}
