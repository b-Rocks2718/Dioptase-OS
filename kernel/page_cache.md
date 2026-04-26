## Page Cache

The kernel page cache stores file-backed pages in memory and keys them by inode pointer plus a page offset. It is a simple hash-table cache with a single blocking lock protecting the table. Entries are reference counted and evicted when refcount is 0.

### Supported Page Cache Features

#### Initialization
`page_cache_init()` allocates the hash table, stores the configured bucket count, initializes the cache lock, and clears all buckets to `NULL`.

#### Lookup / Acquire
`page_cache_acquire()` first checks whether the requested page is already cached. On a hit, it increments the entry refcount and returns the existing entry.

On a miss, it allocates a fresh physical page, reads up to one frame from the backing node with `node_read_all()`, zero-fills any remaining bytes in the frame, inserts a new cache entry, and returns it.

The cache key is the tuple of the node's cached inode pointer and the supplied page offset. The hash function is a simple XOR of those values modulo the hash table size.

#### Dirty Tracking
`page_cache_mark_dirty()` searches for the matching cached page and sets `PG_DIRTY` on the physical page. If no matching entry exists, the function panics.

The dirty flag is a conservative software marker. The code does not show any hardware dirty-bit integration.

#### Release / Writeback
`page_cache_release()` decrements the entry refcount. If the refcount remains above zero, the entry stays cached.

When the refcount reaches zero, the entry is removed from the hash chain and freed. Dirty pages are written back first with `node_write_all()` using the entry's recorded `file_bytes` value. Clean pages are discarded without writeback.

### Data Stored Per Entry

- cached inode pointer and page offset key
- pointer to the physical page data
- reference count
- flags, currently only `PAGE_DIRTY`
- `file_bytes`, the number of bytes from the file that belong to this page
- next pointer for hash-chain collision handling

### Locking

- `page_cache_init()` initializes a single blocking lock for the whole cache
- `page_cache_acquire()`, `page_cache_mark_dirty()`, and `page_cache_release()` all acquire that lock while they inspect or mutate the hash table
- `page_cache_lookup()` and `page_cache_insert()` do not lock on their own and are only safe to call while the cache lock is held

### Not Yet Supported

- Eviction or replacement policy
- Per-page synchronization beyond the global cache lock
- Error handling for allocation or I/O failure