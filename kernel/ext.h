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

#define ICACHE_SIZE 32
#define BCACHE_SIZE 12
// The block cache must reserve enough storage for the largest ext2 block size
// supported by the test harness. Smaller block sizes simply leave unused space.
#define BCACHE_SIZE_BYTES 49152 // 12 cache lines * 4096 byte max ext2 block

#define SD_SECTOR_SIZE_BYTES 512

struct Ext2;

struct CachedInode {
  unsigned inumber;
  struct Inode inode;
  unsigned data_block_count;
  unsigned refcount;
  bool valid;
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

struct BlockCache {
  struct Ext2* fs;
  unsigned tags[BCACHE_SIZE];
  unsigned char ages[BCACHE_SIZE];
  char block_cache[BCACHE_SIZE_BYTES];
  struct BlockingLock lock;
  unsigned block_size;
};

struct Node {
  struct CachedInode* cached;
  unsigned parent_inumber;
  struct Ext2* filesystem;
};

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

  struct BlockingLock inode_alloc_lock; // protects inode allocation state
  struct BlockingLock block_alloc_lock; // protects block allocation state
};

void ext2_init(struct Ext2* fs);

void ext2_destroy(struct Ext2* fs);

// Frees both the ext2 internal allocations and the heap-allocated filesystem
// object itself. Callers must not use this for stack or global `struct Ext2`.
void ext2_free(struct Ext2* fs);

unsigned ext2_get_block_size(struct Ext2* fs);

unsigned ext2_get_inode_size(struct Ext2* fs);

void ext2_expand_path(struct Ext2* fs, char* name, struct RingBuf* path);

struct Node* ext2_enter_dir(struct Ext2* fs, struct Node* dir, struct RingBuf* path);

struct Node* ext2_expand_symlink(struct Ext2* fs, struct Node* parent, struct Node* dir, struct RingBuf* path);


void icache_init(struct InodeCache* cache);

struct CachedInode* icache_get(struct InodeCache* cache, unsigned inumber);

// Inserts a newly created inode into the cache
void icache_insert(struct InodeCache* cache, struct CachedInode* cached);

// Writes the inode data in `inode` back to disk
// Note: cache is write-through
void icache_set(struct InodeCache* cache, struct CachedInode* inode);

// decrement refcount, free if it hits 0
// Note that the caller is responsible for writing back dirty cache entries before releasing them
void icache_release(struct InodeCache* cache, struct CachedInode* inode);

void icache_destroy(struct InodeCache* cache);

void icache_free(struct InodeCache* cache);


void bcache_init(struct BlockCache* cache, unsigned block_size);

void bcache_get(struct BlockCache* cache, unsigned block_num, char* dest);

void bcache_set(struct BlockCache* cache, unsigned block_num, char* src, unsigned offset, unsigned size);


void node_init(struct Node* node, struct CachedInode* cached, unsigned parent_inumber, struct Ext2* fs);

// Releases the inode owned by a heap-allocated node wrapper. The embedded root
// node in `struct Ext2` is owned by `ext2_destroy`, not by this helper.
void node_destroy(struct Node* node);

// Releases a heap-allocated node wrapper and its inode copy.
void node_free(struct Node* node);

unsigned node_size_in_bytes(struct Node* node);

void node_read_block(struct Node* node, unsigned block_num, char* dest);

unsigned node_read_all(struct Node* node, unsigned offset, unsigned size, char* dest);

void node_write_block(struct Node* node, unsigned block_num, char* src, unsigned offset, unsigned size);

unsigned node_write_all(struct Node* node, unsigned offset, unsigned size, char* src);

unsigned short node_get_type(struct Node* node);

bool node_is_dir(struct Node* node);

void node_print_dir(struct Node* node);

bool node_is_file(struct Node* node);

bool node_is_symlink(struct Node* node);

struct Node* node_make_file(struct Node* dir, char* name);

struct Node* node_make_dir(struct Node* dir, char* name);

struct Node* node_make_symlink(struct Node* dir, char* name, char* target);

void node_delete(struct Node* node);

// For symlink nodes only. `dest` must have space for the raw target plus one
// trailing NUL byte because this helper always NUL-terminates the result.
void node_get_symlink_target(struct Node* node, char* dest);

unsigned node_get_num_links(struct Node* node); // num hard links

unsigned node_entry_count(struct Node* node); // for dir nodes only

// Resolves `name` starting from `dir` and returns a heap-allocated node wrapper
// on success (including when the result is the root inode). Callers must release
// any non-NULL result with `node_free(...)`.
struct Node* node_find(struct Node* dir, char* name);

#endif // EXT_H
