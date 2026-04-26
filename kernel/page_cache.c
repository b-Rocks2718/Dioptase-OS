#include "page_cache.h"
#include "heap.h"
#include "physmem.h"
#include "print.h"
#include "debug.h"

struct PageCache page_cache;

// initialize the page cache
void page_cache_init(struct PageCache* cache, unsigned hash_map_size) {
  cache->hash_map = leak(sizeof(struct PageCacheEntry*) * hash_map_size);
  cache->hash_map_size = hash_map_size;
  blocking_lock_init(&cache->lock);
  for (unsigned i = 0; i < hash_map_size; i++) {
    cache->hash_map[i] = NULL;
  }
}

// lookup a page in the page cache by inode and page index
// does not lock the cache,
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

// insert a page into the page cache. should not be called if the page may already exist
// in the cache. does not acquire cache lock
static struct PageCacheEntry* page_cache_insert(struct PageCache* cache, struct Node* node,
                                                unsigned offset, unsigned file_bytes, void* page_data) {
  unsigned hash = ((unsigned)(node->cached) ^ offset) % cache->hash_map_size;
  struct PageCacheEntry* new_entry = malloc(sizeof(struct PageCacheEntry));

  // Keep the inode cache entry alive while this page-cache entry exists.
  node->cached->refcount += 1;
  new_entry->key.inode = node->cached;
  new_entry->key.offset = offset;
  new_entry->page_data = page_data;
  new_entry->file_bytes = file_bytes;

  new_entry->next = cache->hash_map[hash];
  cache->hash_map[hash] = new_entry;

  return new_entry;
}

// lookup a page if it is in the cache, insert into cache if not
struct PageCacheEntry* page_cache_acquire(struct PageCache* cache, struct Node* node, unsigned offset, unsigned file_bytes) {
  blocking_lock_acquire(&cache->lock);

  struct PageCacheEntry* entry = page_cache_lookup(cache, node, offset);
  if (entry) {
    struct Page* page = get_page(entry->page_data);
    physmem_page_lock(page); // TODO lock ordering
    blocking_lock_release(&cache->lock);
    return entry;
  }

  void* page_data = physmem_alloc(); // allocate a new page (pinned)

  // load the page from disk into the newly allocated page_data
  unsigned bytes_read = node_read_all(node, offset, FRAME_SIZE, page_data);

  // zero remaining bytes
  for (int i = bytes_read; i < FRAME_SIZE; i++) {
    ((char*)page_data)[i] = 0;
  }

  entry = page_cache_insert(cache, node, offset, file_bytes, page_data);

  struct Page* page = get_page(page_data);
  page->cache_entry = entry;
  physmem_page_lock(page); // TODO this should never block
  physmem_clear_page_flags(page, PG_PINNED);

  blocking_lock_release(&cache->lock);

  return entry;
}

void page_cache_mark_dirty(struct PageCache* cache, struct Node* node, unsigned offset) {
  unsigned hash = ((unsigned)(node->cached) ^ offset) % cache->hash_map_size;

  blocking_lock_acquire(&cache->lock);

  struct PageCacheEntry* entry = page_cache_lookup(cache, node, offset);
  if (entry != NULL) {
    struct Page* page = get_page(entry->page_data);
    physmem_page_lock(page);
    physmem_set_page_flags(page, PG_DIRTY);
    physmem_page_unlock(page);
    blocking_lock_release(&cache->lock);
  } else {
    panic("page_cache_mark_dirty: missing cache entry for dirty page.\n");
  }
}

/*
// release a page from the page cache
// decrementing its reference count and freeing it if the count reaches zero
void page_cache_release(struct PageCache* cache, struct Node* node, unsigned offset) {
  unsigned hash = ((unsigned)(node->cached) ^ offset) % cache->hash_map_size;
  blocking_lock_acquire(&cache->lock);
  struct PageCacheEntry* entry = cache->hash_map[hash];
  struct PageCacheEntry* prev = NULL;
  while (entry) {
    if (entry->key.inode == node->cached && entry->key.offset == offset) {
      if (entry->refcount > 1) {
        // still live reference, just decrement refcount
        entry->refcount--;
      } else {
        // no more references, remove from hash map and free
        if (prev) {
          prev->next = entry->next;
        } else {
          cache->hash_map[hash] = entry->next;
        }

        // no need to write back clean pages
        if (entry->flags & PAGE_DIRTY) {
          node_write_all(node, offset, entry->file_bytes, entry->page_data);
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
*/

void page_cache_destroy(struct PageCache* cache) {
  blocking_lock_acquire(&cache->lock);
  for (unsigned i = 0; i < cache->hash_map_size; i++) {
    struct PageCacheEntry* entry = cache->hash_map[i];
    while (entry != NULL) {
      // TODO check dirty bits and write back
      struct PageCacheEntry* to_delete = entry;
      entry = entry->next;
      icache_release(&fs.icache, to_delete->key.inode);
      physmem_free(to_delete->page_data);
      free(to_delete);
    }
  }

  blocking_lock_release(&cache->lock);
}

// NOT SYNCHRONIZED; testing purposes only
void page_cache_flush_all(struct PageCache* cache) {
  blocking_lock_acquire(&cache->lock);

  for (unsigned i = 0; i < cache->hash_map_size; i++) {
    struct PageCacheEntry* entry = cache->hash_map[i];
    while (entry != NULL) {
      struct Page* page = get_page(entry->page_data);
      physmem_page_lock(page);
      if (page->flags & PG_DIRTY) {
        struct Node* node = malloc(sizeof(struct Node));
        node_init(node, entry->key.inode, EXT2_BAD_INO, &fs); // hopefully we don't actually need the parent inumber...
        node_write_all(node, entry->key.offset, entry->file_bytes, entry->page_data);
        free(node);
        // say("flushed 1 entry\n", NULL);
      }
      physmem_page_unlock(page);
      entry = entry->next;
    }
  }

  blocking_lock_release(&cache->lock);
}