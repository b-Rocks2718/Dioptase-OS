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

  // Number of bytes, from the start of this page, that a final dirty release
  // may write back. Acquiring a clean/read-only alias never changes this
  // extent; only page_cache_mark_dirty() and serialized shrink update it.
  unsigned writeback_bytes;

  struct PageCacheEntry* next;
};

// page cache storing file pages
struct PageCache {
  struct PageCacheEntry** hash_map;
  unsigned hash_map_size;

  struct BlockingLock lock;
};

// initialize the page cache
void page_cache_init(struct PageCache* cache);

// destroy page-cache synchronization after all mappings/cache users are gone
void page_cache_destroy(struct PageCache* cache);

/*
 * Acquire one page-aligned file page, loading and zero-filling it on a miss.
 * This operation may block and must not run in interrupt context. It retains
 * page_cache.lock while entering the inode read path, establishing the global
 * page-cache-before-inode lock order used by every operation in this module.
 * Acquisition does not authorize any writeback extent: read-only/private
 * users must not enlarge a dirty shared mapper's eventual file write.
 */
struct PageCacheEntry* page_cache_acquire(struct PageCache* cache, struct Node* node, 
    unsigned offset);

/*
 * Conservatively mark one cached page dirty and max-merge the byte extent
 * exposed by this writable shared mapping. `exposed_bytes` must be in
 * 1..FRAME_SIZE. The ISA does not expose a usable dirty transition here, so a
 * writable fault authorizes the complete logical mapping extent even if user
 * code ultimately leaves some of those bytes unchanged.
 */
void page_cache_mark_dirty(struct PageCache* cache, struct Node* node,
    unsigned offset, unsigned exposed_bytes);

/*
 * Shrink one regular file while serializing it against page acquisition,
 * dirty-extent publication, and final writeback. Lock order is always
 * page_cache.lock followed by node->cached->lock; ext code must not call this
 * API while already holding an inode lock. On success, dirty pages wholly at
 * or beyond the new EOF are made clean, while a straddling page keeps only its
 * prefix writeback extent. Returns false, without changing cache metadata, if
 * target_size would grow the file.
 */
bool page_cache_shrink_file(struct PageCache* cache, struct Node* node,
    unsigned target_size);

// release a page from the page cache
// decrementing its reference count and freeing it if the count reaches zero
void page_cache_release(struct PageCache* cache, struct Node* node, unsigned offset);

#endif // PAGE_CACHE_H
