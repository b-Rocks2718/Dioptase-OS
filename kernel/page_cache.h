#ifndef PAGE_CACHE_H
#define PAGE_CACHE_H

#include "ext.h"
#include "blocking_lock.h"

// Page Cache is indexed by inode and page index
struct PageCacheKey {
  struct CachedInode* inode;
  unsigned offset;
};

#define PAGE_DIRTY 0x1

// metadata for the page cache entry
struct PageCacheEntry {
  struct PageCacheKey key;
  void* page_data;

  unsigned refcount;
  unsigned flags;

  // how many bytes of the file this page actually contains
  unsigned file_bytes;

  struct PageCacheEntry* next;
};

// page cache storing file pages
struct PageCache {
  struct PageCacheEntry** hash_map;
  unsigned hash_map_size;

  struct BlockingLock lock;
};

// initialize the page cache
void page_cache_init(struct PageCache* cache, unsigned hash_map_size);

// lookup a page if it is in the cache, insert into cache if not
struct PageCacheEntry* page_cache_acquire(struct PageCache* cache, struct Node* node, 
    unsigned offset, unsigned file_bytes);

// Conservatively mark one cached page dirty. Shared writable mappings call this
// when they expose a cache page directly to userspace because the ISA does not
// currently provide a hardware dirty bit for later writeback decisions.
void page_cache_mark_dirty(struct PageCache* cache, struct Node* node, unsigned offset);

// release a page from the page cache
// decrementing its reference count and freeing it if the count reaches zero
void page_cache_release(struct PageCache* cache, struct Node* node, unsigned offset);

#endif // PAGE_CACHE_H
