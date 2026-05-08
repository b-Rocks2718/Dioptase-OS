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
// TODO should we maybe just return the ppn or a page object? should the page object contain a PPN
struct PageCacheEntry* page_cache_acquire(struct PageCache* cache, struct Node* node, unsigned offset, unsigned file_bytes) {
  while (true) {
    blocking_lock_acquire(&cache->lock);

    struct PageCacheEntry* entry = page_cache_lookup(cache, node, offset);
    if (entry != NULL) {
      // Lock page
      void* page_data = entry->page_data;
      struct Page* page = get_page(page_data, "get page - cache acquire hit");
      blocking_lock_release(&cache->lock); // Can't hold the cache lock while acquiring a page lock
      physmem_page_lock(page);

      // Revalidate
      blocking_lock_acquire(&cache->lock);
      struct PageCacheEntry* verify = page_cache_lookup(cache, node, offset);
      if ((verify != NULL) && (verify->page_data == page_data) && !(page->flags & PG_PINNED)) { // Success!
        // say("Released cache lock - revalidate success\n", NULL);
        blocking_lock_release(&cache->lock);
        assert(verify == page->cache_entry, "page cache mismatch");

        // Update file_bytes
        struct CachedInode* inode = verify->key.inode;
        if (file_bytes > verify->file_bytes) {
          verify->file_bytes = file_bytes;
        }
        blocking_lock_acquire(&inode->lock);
        if (offset + file_bytes > inode->inode.size) {
          inode->inode.size = offset + file_bytes;
        }
        blocking_lock_release(&inode->lock);

        return verify;
      } else { // Fail (it got evicted, or it's still being paged in)
        blocking_lock_release(&cache->lock);
        physmem_page_unlock(page);
        continue; // If revalidation failed, loop again
      }
    }

    // Insert into the page cache
    void* page_data = physmem_alloc(); // allocate a new page (pinned)
    entry = page_cache_insert(cache, node, offset, file_bytes, page_data);
    struct Page* page = get_page(page_data, "get page - cache acquire miss");
    page->cache_entry = entry;
    blocking_lock_release(&cache->lock); // Release the cache lock while acquiring other locks
    // We're safe from eviction because the page is pinned
    physmem_page_lock(page);

    // Update file_bytes
    struct CachedInode* inode = entry->key.inode;
    blocking_lock_acquire(&inode->lock);
    if (offset + file_bytes > inode->inode.size) {
      inode->inode.size = offset + file_bytes;
    }
    blocking_lock_release(&inode->lock);

    // load the page from disk into the newly allocated page_data
    unsigned bytes_read = node_read_all(node, offset, FRAME_SIZE, page_data);
    // zero remaining bytes
    for (int i = bytes_read; i < FRAME_SIZE; i++) {
      ((char*)page_data)[i] = 0;
    }
    physmem_clear_page_flags(page, PG_PINNED);
    return entry;
  }
}

void page_cache_remove(struct PageCache* cache, struct PageCacheEntry* entry) {
  unsigned hash = ((unsigned)(entry->key.inode) ^ entry->key.offset) % cache->hash_map_size;
  blocking_lock_acquire(&cache->lock);
  struct PageCacheEntry* curr = cache->hash_map[hash];
  struct PageCacheEntry* prev = NULL;
  while (curr) {
    if (curr == entry) {
      // remove from hash map and free
      if (prev) {
        prev->next = entry->next;
      } else {
        cache->hash_map[hash] = entry->next;
      }
      break;
    }
    prev = curr;
    curr = curr->next;
  }
  blocking_lock_release(&cache->lock);
}

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
      struct Page* page = get_page(entry->page_data, "get page - cache flush all");
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