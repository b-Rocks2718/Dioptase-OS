#ifndef PAGE_CACHE_H
#define PAGE_CACHE_H

#include "ext.h"
#include "blocking_lock.h"

// Page Cache is indexed by inode and page index
struct PageCacheKey {
  struct CachedInode* inode;
  unsigned offset;
};

// metadata for the page cache entry
struct PageCacheEntry {
  struct PageCacheKey key;
  void* page_data; // Pointer to frame

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

extern struct PageCache page_cache;

// initialize the page cache
void page_cache_init(struct PageCache* cache, unsigned hash_map_size);

// lookup a page if it is in the cache, insert into cache if not
struct PageCacheEntry* page_cache_acquire(struct PageCache* cache, struct Node* node,
                                          unsigned offset, unsigned file_bytes);

// Looks up the page if it is in the page cache; returns NULL if not
struct PageCacheEntry* page_cache_acquire_if_present(struct PageCache* cache, struct Node* node, unsigned offset);

// remove a page from the page cache
void page_cache_remove(struct PageCache* cache, struct PageCacheEntry* entry);

void page_cache_destroy(struct PageCache* cache);

void page_cache_flush_all(struct PageCache* cache);
#endif // PAGE_CACHE_H
