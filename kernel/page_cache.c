#include "page_cache.h"
#include "heap.h"
#include "physmem.h"
#include "print.h"
#include "debug.h"

// initialize the page cache
void page_cache_init(struct PageCache* cache){
  static unsigned hash_map_size = 4096; // 16384 bytes
  cache->hash_map = physmem_leak_order(2); // 4096 entries * 4 bytes each = 16384 bytes = 2^2 pages
  cache->hash_map_size = hash_map_size;
  blocking_lock_init(&cache->lock);
  for(unsigned i = 0; i < hash_map_size; i++){
    cache->hash_map[i] = NULL;
  }
}

// to be called only from kernel_shutdown
void page_cache_destroy(struct PageCache* cache){
  assert(cache != NULL, "page_cache_destroy: cache is NULL.\n");
  blocking_lock_destroy(&cache->lock);
}

// lookup a page in the page cache by inode and page index, incrementing its reference count if found
// does not lock the cache, 
static struct PageCacheEntry* page_cache_lookup(struct PageCache* cache, struct Node* node, unsigned offset){
  unsigned hash = ((unsigned)(node->cached) ^ offset) % cache->hash_map_size;
  struct PageCacheEntry* entry = cache->hash_map[hash];
  // iterate linked list until we find a match
  while (entry){
    if(entry->key.inode == node->cached && entry->key.offset == offset){
      entry->refcount++;
      return entry;
    }
    entry = entry->next;
  }
  return NULL;
}

// Insert a clean page into the cache. The caller holds cache->lock and has
// already established that no matching entry exists.
static struct PageCacheEntry* page_cache_insert(struct PageCache* cache, struct Node* node, 
    unsigned offset, void* page_data){
  unsigned hash = ((unsigned)(node->cached) ^ offset) % cache->hash_map_size;
  struct PageCacheEntry* new_entry = malloc(sizeof(struct PageCacheEntry));
  new_entry->key.inode = node->cached;
  new_entry->key.offset = offset;
  new_entry->page_data = page_data;
  new_entry->refcount = 1;
  new_entry->flags = 0;
  new_entry->writeback_bytes = 0;

  new_entry->next = cache->hash_map[hash];
  cache->hash_map[hash] = new_entry;

  return new_entry;
}

// Lookup a page if it is in the cache, or load and insert a clean page on a
// miss. Holding the blocking cache lock across node_read_all() deliberately
// orders all cache/inode operations as page cache -> inode. Sequential
// consistency plus that common lock makes cache publication atomic to other
// cores without disabling interrupts around blocking storage I/O.
struct PageCacheEntry* page_cache_acquire(struct PageCache* cache,
    struct Node* node, unsigned offset){
  blocking_lock_acquire(&cache->lock);

  struct PageCacheEntry* entry = page_cache_lookup(cache, node, offset);
  if (entry){
    blocking_lock_release(&cache->lock);
    return entry;
  }

  void* page_data = physmem_alloc(); // allocate a new page
  if (page_data == NULL){
    blocking_lock_release(&cache->lock);
    return NULL;
  }

  // load the page from disk into the newly allocated page_data
  unsigned bytes_read = node_read_all(node, offset, FRAME_SIZE, page_data);

  // zero remaining bytes
  for (int i = bytes_read; i < FRAME_SIZE; i++){
    ((char*)page_data)[i] = 0;
  }

  entry = page_cache_insert(cache, node, offset, page_data);

  blocking_lock_release(&cache->lock);

  return entry;
}

void page_cache_mark_dirty(struct PageCache* cache, struct Node* node,
    unsigned offset, unsigned exposed_bytes){
  if (exposed_bytes == 0 || exposed_bytes > FRAME_SIZE){
    int args[3] = {
      (int)node->cached->inumber,
      (int)offset,
      (int)exposed_bytes,
    };
    say("| page cache: invalid dirty extent inode=%u offset=%u exposed_bytes=%u\n",
      args);
    panic("page_cache_mark_dirty: writable mapping exposed an invalid page extent.\n");
  }

  unsigned hash = ((unsigned)(node->cached) ^ offset) % cache->hash_map_size;

  blocking_lock_acquire(&cache->lock);
  struct PageCacheEntry* entry = cache->hash_map[hash];
  while (entry){
    if (entry->key.inode == node->cached && entry->key.offset == offset){
      entry->flags |= PAGE_DIRTY;
      // Only writable mappings publish an extent. A later writable alias may
      // expose more of this page and must extend the eventual writeback, while
      // a wider read-only/private acquisition has no effect.
      if (entry->writeback_bytes < exposed_bytes){
        entry->writeback_bytes = exposed_bytes;
      }
      blocking_lock_release(&cache->lock);
      return;
    }
    entry = entry->next;
  }
  blocking_lock_release(&cache->lock);

  int args[2] = {(int)node->cached->inumber, (int)offset};
  say("| page cache: dirty publication missed inode=%u offset=%u\n", args);
  panic("page_cache_mark_dirty: missing cache entry for dirty page.\n");
}

bool page_cache_shrink_file(struct PageCache* cache, struct Node* node,
    unsigned target_size){
  assert(cache != NULL, "page_cache_shrink_file: cache is NULL.\n");
  assert(node != NULL, "page_cache_shrink_file: node is NULL.\n");
  assert(node_is_file(node),
    "page_cache_shrink_file: node must be a regular file.\n");

  /*
   * Preconditions:
   * - the caller owns a live Node wrapper and does not hold its inode lock;
   * - this is ordinary kernel-thread or trap-continuation context, not an
   *   interrupt handler. BlockingLock/storage I/O may explicitly block; the
   *   scheduler preserves that thread's IMR across the context switch.
   *
   * Postcondition on success:
   * - the on-disk/in-memory inode EOF and every matching live page-cache
   *   writeback extent reflect one serialized truncate operation;
   * - no final release of an already-dirty page can restore bytes beyond that
   *   EOF unless a later writable fault explicitly republishes a wider extent.
   *
   * The page-cache lock is intentionally outermost. page_cache_acquire() and
   * page_cache_release() already use this same page-cache -> inode order, and
   * no ext path acquires the page-cache lock while holding an inode lock.
   */
  blocking_lock_acquire(&cache->lock);

  if (!node_shrink(node, target_size)){
    blocking_lock_release(&cache->lock);
    return false;
  }

  for (unsigned bucket = 0; bucket < cache->hash_map_size; ++bucket){
    for (struct PageCacheEntry* entry = cache->hash_map[bucket];
        entry != NULL; entry = entry->next){
      if (entry->key.inode != node->cached){
        continue;
      }

      if (entry->key.offset >= target_size){
        // The complete cached page begins at or beyond the new EOF. Its bytes
        // may remain visible to existing mappings, but an old dirty release
        // must not recreate the truncated file range.
        entry->flags &= ~PAGE_DIRTY;
        entry->writeback_bytes = 0;
        continue;
      }

      unsigned prefix_bytes = target_size - entry->key.offset;
      if (entry->writeback_bytes > prefix_bytes){
        // Preserve dirty data before EOF while forbidding this already-live
        // entry from writing its truncated suffix back later.
        entry->writeback_bytes = prefix_bytes;
      }
    }
  }

  blocking_lock_release(&cache->lock);
  return true;
}

// release a page from the page cache
// decrementing its reference count and freeing it if the count reaches zero
void page_cache_release(struct PageCache* cache, struct Node* node, unsigned offset){
  unsigned hash = ((unsigned)(node->cached) ^ offset) % cache->hash_map_size;
  blocking_lock_acquire(&cache->lock);
  struct PageCacheEntry* entry = cache->hash_map[hash];
  struct PageCacheEntry* prev = NULL;
  while (entry){
    if (entry->key.inode == node->cached && entry->key.offset == offset){
      if (entry->refcount > 1){
        // still live reference, just decrement refcount
        entry->refcount--;
      } else {
        // no more references, remove from hash map and free
        if (prev){
          prev->next = entry->next;
        } else {
          cache->hash_map[hash] = entry->next;
        }

        // no need to write back clean pages
        if (entry->flags & PAGE_DIRTY){
          assert_always(entry->writeback_bytes > 0 &&
              entry->writeback_bytes <= FRAME_SIZE,
            "page_cache_release: dirty entry has an invalid writeback extent.\n");
          node_write_all(node, offset, entry->writeback_bytes,
            entry->page_data);
        }

        physmem_free(entry->page_data);
        free(entry);
      }
      break;
    }
    prev = entry;
    entry = entry->next;
  }
  blocking_lock_release(&cache->lock);
}
