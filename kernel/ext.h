#ifndef EXT_H
#define EXT_H

#include "ext2_structs.h"
#include "atomic.h"
#include "machine.h"
#include "shared.h"
#include "queue.h"
#include "hashmap.h"
#include "blocking_lock.h"
#include "gate.h"

#define BCACHE_SIZE 32

#define SD_SECTOR_SIZE_BYTES 512

struct Ext2;

struct CachedInode {
  unsigned inumber;
  struct Inode inode;
  unsigned data_block_count;
  unsigned refcount;
  bool valid;
  // Once the last directory link is removed, keep the inode cached until the
  // final wrapper releases it. That prevents inode-number reuse and block
  // reclamation from racing with still-live `struct Node*` users.
  bool delete_pending;
  // Serializes inode size, block-tree, link-count, and delete-pending updates.
  struct BlockingLock lock;
  struct Gate valid_gate; // threads waiting for valid == true
};

// A simple write-through cache for inodes
// The cache entries are reference-counted so they can be shared across multiple nodes 
// and safely released when no longer in use
struct InodeCache {
  struct Ext2* fs;
  struct HashMap cache; // inumber -> CachedInode*
  struct BlockingLock lock;
};

// small write-through cache for ext2 logical blocks
struct BlockCache {
  struct Ext2* fs;
  unsigned tags[BCACHE_SIZE]; // cached logical block numbers, or UINT_MAX when empty
  unsigned char ages[BCACHE_SIZE]; // pseudo-LRU ages; 0 is most recently used
  char* block_cache;
  struct BlockingLock lock;
  unsigned block_size;
};

// one wrapper around a cached inode plus traversal context
struct Node {
  struct CachedInode* cached;

  // initialized to EXT2_BAD_INO,
  // filled in when find() traverses directories
  // INVARIANT: any Node* returned by `node_find` has a valid parent_inumber
  // (except root, which keeps EXT2_BAD_INO)
  unsigned parent_inumber;

  struct Ext2* filesystem;
};

// the main ext2 filesystem struct, containing the superblock, block group descriptors, and caches
struct Ext2 {
  struct Superblock superblock;
  struct InodeCache icache;
  struct BlockCache bcache;

  unsigned num_block_groups;

  // number of block groups is generally small
  unsigned bgd_offset;
  struct BGD* bgd_table;

  char** inode_bitmaps;
  char** block_bitmaps;

  struct Node root;

  struct BlockingLock metadata_lock; // protects writes to BGD table, superblock, and bitmaps
  struct BlockingLock inode_lock; // protects inode table
};

// Initializes `fs` from the ext2 rev-0 image on SD drive 1. `fs` must point to
// writable storage that remains valid until `ext2_destroy(...)` or
// `ext2_free(...)` runs.
void ext2_init(struct Ext2* fs);

// Releases all allocations created by `ext2_init(...)`. This invalidates
// `fs->root` and every cached inode or heap-allocated `struct Node` wrapper
// associated with this filesystem instance.
void ext2_destroy(struct Ext2* fs);

// Frees both the ext2 internal allocations and the heap-allocated filesystem
// object itself. Callers must not use this for stack or global `struct Ext2`.
void ext2_free(struct Ext2* fs);

// Returns the ext2 logical block size decoded from the loaded superblock.
// Valid only after `ext2_init(...)`.
unsigned ext2_get_block_size(struct Ext2* fs);

// Returns the on-disk inode size decoded from the loaded superblock. Valid only
// after `ext2_init(...)`.
unsigned ext2_get_inode_size(struct Ext2* fs);

// Internal traversal helpers used by `node_find(...)`. External callers should
// normally prefer the higher-level node API below.
void ext2_expand_path(struct Ext2* fs, char* name, struct RingBuf* path);

struct Node* ext2_enter_dir(struct Ext2* fs, struct Node* dir, struct RingBuf* path);

// replace the current symlink component with its target path and choose the
// correct restart directory for relative vs absolute targets
struct Node* ext2_expand_symlink(struct Ext2* fs, struct Node* parent, struct Node* dir, struct RingBuf* path);


// Internal inode-cache helpers. Cache entries are shared between all wrappers
// that reference one inode and are released by reference count.
void icache_init(struct InodeCache* cache);

// return a shared cached inode, reading it from disk on the first miss
struct CachedInode* icache_get(struct InodeCache* cache, unsigned inumber);

// Inserts a newly created inode into the cache
void icache_insert(struct InodeCache* cache, struct CachedInode* cached);

// Writes the inode data in `inode` back to disk
// Note: cache is write-through
void icache_set(struct InodeCache* cache, struct CachedInode* inode);

// decrement refcount, free if it hits 0
// Note that the caller is responsible for writing back dirty cache entries before releasing them
void icache_release(struct InodeCache* cache, struct CachedInode* inode);

// destroy the inode cache and release its internal storage
void icache_destroy(struct InodeCache* cache);

// destroy the inode cache and free the cache struct
void icache_free(struct InodeCache* cache);


// Internal block-cache helpers. The block cache is write-through and keyed by
// ext2 logical block number.
void bcache_init(struct BlockCache* cache, unsigned block_size);

// read one cached logical block into dest
void bcache_get(struct BlockCache* cache, unsigned block_num, char* dest);

// write one logical block range through the cache and out to disk
void bcache_set(struct BlockCache* cache, unsigned block_num, char* src, unsigned offset, unsigned size);

void bcache_destroy(struct BlockCache* cache);

// Initializes one wrapper around a shared cached inode. Callers may create
// multiple wrappers for one inode; ownership of the cached inode remains shared
// through the inode cache reference count.
void node_init(struct Node* node, struct CachedInode* cached, unsigned parent_inumber, struct Ext2* fs);

// Creates a new heap-allocated wrapper around the same cached inode as `node`
// and increments the cached inode's reference count so that the inode remains
// valid for the lifetime of the new wrapper. Returns NULL if `node` is NULL
struct Node* node_clone(struct Node* node);

// Releases the inode owned by a heap-allocated node wrapper. The embedded root
// node in `struct Ext2` is owned by `ext2_destroy`, not by this helper.
void node_destroy(struct Node* node);

// Releases a heap-allocated node wrapper and its inode copy.
void node_free(struct Node* node);

// Returns the current logical size of the inode in bytes.
unsigned node_size_in_bytes(struct Node* node);

// Reads one already-allocated logical block from `node` into `dest`. Callers
// must only request block numbers that actually exist in the inode's current
// data-block tree; `node_read_all(...)` is the safe API for EOF-clamped reads.
void node_read_block(struct Node* node, unsigned block_num, char* dest);

// Reads up to `size` bytes starting at `offset`. The read shortens at EOF and
// returns the actual byte count copied, which may be smaller than `size`.
unsigned node_read_all(struct Node* node, unsigned offset, unsigned size, char* dest);

// Writes into one already-allocated logical block. Callers must ensure the
// logical block exists before calling this helper; `node_write_all(...)` is the
// API that grows regular files and long symlinks as needed.
void node_write_block(struct Node* node, unsigned block_num, char* src, unsigned offset, unsigned size);

// Writes `size` bytes starting at `offset` and grows the inode if needed.
// Supported only for regular files and symlinks. The returned count matches the
// requested write size on success.
unsigned node_write_all(struct Node* node, unsigned offset, unsigned size, char* src);

// return the ext2 inode type bits
unsigned short node_get_type(struct Node* node);

// report whether the inode is a directory
bool node_is_dir(struct Node* node);

// print each live directory entry name on its own line
void node_print_dir(struct Node* node);

// report whether the inode is a regular file
bool node_is_file(struct Node* node);

// report whether the inode is a symbolic link
bool node_is_symlink(struct Node* node);

// Creates one regular file entry inside `dir`. `dir` must be a directory, and
// `name` must be one non-empty directory-entry component without '/' and must
// not be "." or "..". Returns a heap-owned wrapper for the new inode, or NULL
// if the basename already exists in `dir` or `dir` has already been unlinked
// and is only being kept alive by existing wrappers.
struct Node* node_make_file(struct Node* dir, char* name);

// Creates one subdirectory inside `dir`. The same basename rules as
// `node_make_file(...)` apply. The new directory is initialized with "." and
// ".." and returned as a heap-owned wrapper, or NULL on duplicate basename or
// when `dir` has already been unlinked.
struct Node* node_make_dir(struct Node* dir, char* name);

// Creates one symlink entry inside `dir`. `name` follows the same basename
// rules as other create helpers. `target` is stored verbatim and may be either
// absolute or relative. Returns NULL on duplicate basename or when `dir` has
// already been unlinked.
struct Node* node_make_symlink(struct Node* dir, char* name, char* target);

// Renames one existing entry inside the supplied parent directory. This helper
// only supports same-directory renames: `old_name` and `new_name` must both be
// single non-empty entry names without '/' and may not be "." or "..".
// Renaming onto an existing target name is rejected, while renaming a name to
// itself is a no-op.
void node_rename(struct Node* dir, char* old_name, char* new_name);

// Deletes one entry from `dir`. `name` must be one non-empty entry component
// without '/' and may not be "." or "..". Directory targets must already be
// empty except for "." and "..". If this removes the final directory link, the
// pathname disappears immediately but block/inode reclamation is deferred until
// every live wrapper for that inode has been released.
void node_delete(struct Node* dir, char* name);

// For symlink nodes only. `dest` must have space for the raw target plus one
// trailing NUL byte because this helper always NUL-terminates the result.
void node_get_symlink_target(struct Node* node, char* dest);

// Returns the inode link count from the ext2 metadata.
unsigned node_get_num_links(struct Node* node);

// Counts live directory entries in one directory inode. Removed entries with
// inode 0 are ignored.
unsigned node_entry_count(struct Node* node);

// Resolves `name` starting from `dir`, follows symlinks, and returns a
// heap-allocated node wrapper on success. Absolute paths restart from the ext2
// root. An empty `name` resolves to the current node while still returning a
// heap-owned wrapper. Callers must release any non-NULL result with
// `node_free(...)`.
struct Node* node_find(struct Node* dir, char* name);

#endif // EXT_H
