## Page Cache

The kernel page cache serves as a canonicalizing mapping for accessing files, as well as a cache for file-backed pages to provide faster access.

### Structure

The cache is a simple hash-table mapping from `(inode pointer, page offset)` to a cached page entry, which contains a pointer to the physical page data and the number of bytes from the file that belong to this page. The cache is protected by a single global blocking lock and uses chaining for collision handling.

The cache key is the tuple of the node's cached inode pointer and the supplied page offset. The hash function is a simple XOR of those values modulo the hash table size.

### Supported Page Cache Features

#### Initialization
`page_cache_init()` allocates the hash table, stores the configured bucket count, initializes the cache lock, and clears all buckets to `NULL`.

#### Lookup / Acquire
`page_cache_acquire()` returns the cached page entry, locking the backing page. 

If the entry is already cached. On a hit, it returns the existing entry. On a miss, it allocates a fresh physical page, inserts a new entry into the cache, locks the backing page, reads up to one frame from the backing node with `node_read_all()`, zero-fills any remaining bytes in the frame, and returns the newly created entry.
- On a hit, the page cache lock must be released in order to claim the page lock; this creates a window for eviction and necessitates revalidation
- On a miss, the freshly allocated frame is pinned, and thus not evictable

The caller is expected to update the page's metadata (mostly the reverse mapping) and release the page lock when done.

#### Release / Writeback
Page cache entries are released when the backing page is evicted by `page_evict()`. Dirty pages are written back first with `node_write_all()` using the entry's recorded `file_bytes` value. Clean pages are discarded without writeback.

#### Eviction
`page_evict()` can be called on the metadata of a frame to free the frame and evict the cache entry. It may only be called if the backing frame is not pinned, and if the caller holds the page lock.

Page eviction uses the reverse mapping (stored in `page->refs`) to find and invalidate all virtual mappings before reclaiming the physical frame. The pathway:
- Lock the INode (prevents re-caching of the page while eviction writeback is in-flight)
- Remove the entry from the cache
- For each `PageRef` in `page->refs`, invalidate the corresponding PTE and perform a TLB shootdown (see **Reverse Mapping** in vmem.md)
- If the page is dirty (checked via `page->flags`), write it back to the backing node with `node_write_all()` using the entry's recorded `file_bytes` value
- Free the backing frame via `physmem_free()`

Eviction is currently invoked only by explicit caller requests; there is no automatic reclamation policy. // TODO: add an eviction policy

### Data Stored Per Entry

- cached inode pointer and page offset key
- pointer to the physical page data
- `file_bytes`, the number of bytes from the file that belong to this page
- next pointer for hash-chain collision handling

### Locking

- `page_cache_init()` initializes a single blocking lock for the whole cache
- `page_cache_acquire()` acquires that lock while it inspects or mutates the hash table
- `page_cache_lookup()` and `page_cache_insert()` do not lock on their own and are only safe to call while the cache lock is held

#### Lock Ordering
Locks must be acquired in order of decreasing granularity (i.e. you cannot hold a coarser lock while contesting for a finer lock). Specifically, locks must be acquired in the following order:
- Page lock
- INode lock
- Global page cache lock

### Not Yet Supported
- Page replacement policy
- Error handling for allocation or I/O failure