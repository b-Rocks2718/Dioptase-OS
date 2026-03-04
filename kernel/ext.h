#ifndef EXT_H
#define EXT_H

#include "ext2_structs.h"
#include "atomic.h"
#include "machine.h"
#include "shared.h"
#include "queue.h"
#include "blocking_lock.h"

#define ICACHE_SIZE 16
#define BCACHE_SIZE 12
// The block cache must reserve enough storage for the largest ext2 block size
// supported by the test harness. Smaller block sizes simply leave unused space.
#define BCACHE_SIZE_BYTES 49152 // 12 cache lines * 4096 byte max ext2 block

struct Ext2;

struct InodeCache {
  struct Ext2* fs;
  unsigned tags[ICACHE_SIZE]; // tags = array of inode numbers
  unsigned char ages[ICACHE_SIZE];
  struct Inode inode_cache[ICACHE_SIZE];
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
  struct Inode* inode;
  unsigned inumber;
  // Parent directory inode number. For the root node, this is the root inode
  // itself. Symlink traversal uses this to resolve relative targets when a
  // caller starts a lookup from a symlink node.
  unsigned parent_inumber;
  struct Ext2* filesystem;
};

struct Ext2 {
  struct Superblock superblock;
  struct InodeCache icache;
  struct BlockCache bcache;

  unsigned num_block_groups;

  // number of block groups is generally small
  struct BGD* bgd_table;

  struct Node root;
};

void ext2_init(struct Ext2* fs);

void ext2_destroy(struct Ext2* fs);

// Frees both the ext2 internal allocations and the heap-allocated filesystem
// object itself. Callers must not use this for stack or global `struct Ext2`.
void ext2_free(struct Ext2* fs);

unsigned ext2_get_block_size(struct Ext2* fs);

unsigned ext2_get_inode_size(struct Ext2* fs);

// Resolves `name` starting from `dir` and returns a heap-allocated node wrapper
// on success (including when the result is the root inode). Callers must release
// any non-NULL result with `node_free(...)`.
struct Node* ext2_find(struct Ext2* fs, struct Node* dir, char* name);

struct Node* ext2_make_file(struct Ext2* fs, struct Node* dir, char* name);

struct Node* ext2_make_dir(struct Ext2* fs, struct Node* dir, char* name);

struct Node* ext2_make_symlink(struct Ext2* fs, struct Node* dir, char* name, char* target);

void ext2_expand_path(struct Ext2* fs, char* name, struct RingBuf* path);

struct Node* ext2_enter_dir(struct Ext2* fs, struct Node* dir, struct RingBuf* path);

struct Node* ext2_expand_symlink(struct Ext2* fs, struct Node* parent, struct Node* dir, struct RingBuf* path);


void icache_init(struct InodeCache* cache);

struct Inode* icache_get(struct InodeCache* cache, unsigned inumber);


void bcache_init(struct BlockCache* cache, unsigned block_size);

void bcache_get(struct BlockCache* cache, unsigned block_num, char* dest);


void node_init(struct Node* node, unsigned inumber, struct Inode* inode, struct Ext2* fs);

// Releases the inode owned by a heap-allocated node wrapper. The embedded root
// node in `struct Ext2` is owned by `ext2_destroy`, not by this helper.
void node_destroy(struct Node* node);

// Releases a heap-allocated node wrapper and its inode copy.
void node_free(struct Node* node);

unsigned node_size_in_bytes(struct Node* node);

void node_read_block(struct Node* node, unsigned block_num, char* dest);

unsigned node_read_all(struct Node* node, unsigned offset, unsigned size, char* dest);

unsigned short node_get_type(struct Node* node);

bool node_is_dir(struct Node* node);

bool node_is_file(struct Node* node);

bool node_is_symlink(struct Node* node);

// For symlink nodes only. `dest` must have space for the raw target plus one
// trailing NUL byte because this helper always NUL-terminates the result.
void node_get_symlink_target(struct Node* node, char* dest);

unsigned node_get_num_links(struct Node* node); // num hard links

unsigned node_entry_count(struct Node* node); // for dir nodes only

#endif // EXT_H
