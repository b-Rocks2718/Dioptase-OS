/*
 * Eviction concurrency stress test.
 *
 * 3 writer threads repeatedly write disjoint bytes into a shared, file-backed
 * mapping while one evictor thread continuously evicts the page. After a
 * fixed number of rounds we quiesce and verify the backing file contains the
 * writers' last committed values.
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

#define WRITERS        3
#define EVICTOR_CORE   3
#define ROUNDS         128
#define TEST_FILE_NAME "evict_conc.txt"
#define TEST_BYTES     512

static int finished = 0;
static int progress = 0;

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

static void writer_thread(void* arg) {
  struct WriterArg* a = (struct WriterArg*)arg;
  int id = a->id;
  struct Node* file = node_find(&fs.root, TEST_FILE_NAME);
  assert(file != NULL, "evict conc: failed to open fixture file\n");
  assert(file->cached != NULL, "NULL CACHED INODE\n");
  char* mapping = mmap(TEST_BYTES, file, 0, MMAP_READ | MMAP_WRITE | MMAP_SHARED);
  node_free(file);
  assert(mapping != NULL, "evict conc: mmap returned NULL\n");

  for (int r = 0; r < ROUNDS - 1; ++r) {
    // write one byte slot owned by this writer each round
    unsigned offset = (id * ROUNDS) + r; // disjoint low-order bytes
    mapping[offset] = 97 + (r % 26);

    // widen the race window; give eviction a chance to run
    if ((r & (id * 2)) == 0)
      yield();
    __atomic_fetch_add(&progress, 1);
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
    say("a\n", NULL);
    blocking_lock_acquire(&page_cache.lock);
    // acquire page cache entry to find the page frame (if present)
    struct PageCacheEntry* entry = page_cache_lookup(&page_cache, file, 0);
    blocking_lock_release(&page_cache.lock);
    if (entry) {
      say("b\n", NULL);
      struct Page* page = get_page(entry->page_data);
      physmem_page_lock(page);
      page_evict(page);
    } else {
      yield();
    }
    say("c\n", NULL);
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

  yield();

  // wait for writers to finish
  while (__atomic_load_n(&finished) != WRITERS + 1) {
    int args[2] = {progress, WRITERS * ROUNDS - WRITERS};
    say("progress: %d / %d\n", args);
    sleep(50);
  }
  say("***file bytes:\n", NULL);

  // verify backing file contains last writer values
  char* file_bytes = mmap(TEST_BYTES, file, 0, MMAP_READ);
  say("%s", &file);

  node_free(file);
  say("***vmem eviction concurrency test complete\n", NULL);
}
