#include "ext.h"
#include "sd_driver.h"
#include "print.h"
#include "debug.h"
#include "heap.h"
#include "string.h"

// The SD driver exposes the disk as fixed 512-byte sectors, while ext2 uses
// larger logical blocks. All on-disk address conversions must go through this
// unit boundary explicitly.
#define SD_SECTOR_SIZE_BYTES 512

void ext2_init(struct Ext2* fs){
  // start by reading superblock
  int rc = sd_read_blocks(SD_DRIVE_1, 2, 2, &fs->superblock);
  assert(rc == 0, "ext2_init: failed to read ext2 superblock.\n");

  // check this is an ext2 file system
  assert(fs->superblock.magic == 0xEF53, "Not an ext2 file system\n");
  
  // read in the block group descriptor table
  
  unsigned bgd_offset;

  unsigned block_size = ext2_get_block_size(fs);

  if (block_size >= 2048){
    bgd_offset = block_size;
  } else {
    bgd_offset = 2048;
  }

  // Assumes there is at least 1 block
  fs->num_block_groups = (fs->superblock.blocks_count - 1) / fs->superblock.blocks_per_group + 1;

  unsigned bgd_table_bytes = fs->num_block_groups * sizeof(struct BGD);
  unsigned bgd_table_sectors = (bgd_table_bytes + SD_SECTOR_SIZE_BYTES - 1) / SD_SECTOR_SIZE_BYTES;
  // The SD layer reads complete sectors, so the backing buffer must be rounded
  // up even if the descriptor table itself is smaller.
  fs->bgd_table = malloc(bgd_table_sectors * SD_SECTOR_SIZE_BYTES);
  rc = sd_read_blocks(SD_DRIVE_1, bgd_offset / SD_SECTOR_SIZE_BYTES,
    bgd_table_sectors, (char*)fs->bgd_table);
  assert(rc == 0, "ext2_init: failed to read block group descriptor table.\n");

  fs->icache.fs = fs;
  icache_init(&fs->icache);
  fs->bcache.fs = fs;
  bcache_init(&fs->bcache, block_size);

  // Read the root inode through the inode cache so bootstrap uses the same
  // inode size and block-size rules as every other inode lookup.
  struct Inode* root_inode = icache_get(&fs->icache, EXT2_ROOT_INO);

  // The ext2 root inode must always be a directory.
  assert((root_inode->mode & 0xF000) == EXT2_S_IFDIR,
    "ext2_init: root inode is not a directory.\n");

  node_init(&fs->root, EXT2_ROOT_INO, root_inode, fs);
}

void ext2_destroy(struct Ext2* fs){
  free(fs->bgd_table);
  free(fs->root.inode);
}

void ext2_free(struct Ext2* fs){
  ext2_destroy(fs);
  free(fs);
}

unsigned ext2_get_block_size(struct Ext2* fs){
  return 1024 << fs->superblock.log_block_size;
}

unsigned ext2_get_inode_size(struct Ext2* fs){
  return fs->superblock.inode_size;
}

void ext2_expand_path(struct Ext2* fs, char* name, struct RingBuf* path){
  unsigned name_len = strlen(name);
  unsigned start = 0;

  // Queue path components in left-to-right order so `ringbuf_remove()` returns
  // them in traversal order.
  while (start < name_len) {
    while (start < name_len && name[start] == '/') {
      start++;
    }

    if (start >= name_len) {
      break;
    }

    unsigned end = start;
    while (end < name_len && name[end] != '/') {
      end++;
    }

    unsigned size = (end - start) + 1;
    char* component = malloc(size);
    memcpy(component, name + start, size - 1);
    component[size - 1] = 0;

    assert(ringbuf_add_front(path, component), "ext2_expand_path: path buffer overflow.\n");
    start = end;
  }
}

// Releases any path components still owned by the traversal queue, then frees
// the queue object itself. `ext2_find` uses this on both success and failure so
// aborted lookups do not leak the remaining path strings.
static void ext2_free_pending_path(struct RingBuf* path){
  if (path == NULL) return;

  while (ringbuf_size(path) > 0) {
    free(ringbuf_remove_back(path));
  }

  ringbuf_free(path);
}

// Reconstruct the directory that contains `node`. This is needed when a caller
// starts a relative lookup from a symlink node, because the traversal must
// resolve that symlink relative to its containing directory rather than the
// symlink inode itself.
static struct Node* ext2_open_parent_dir(struct Node* node){
  struct Ext2* fs = node->filesystem;
  struct Inode* inode = icache_get(&fs->icache, node->parent_inumber);
  struct Node* parent = malloc(sizeof(struct Node));

  node_init(parent, node->parent_inumber, inode, fs);
  assert(node_is_dir(parent),
    "ext2_open_parent_dir: symlink parent inode is not a directory.\n");

  return parent;
}

// Create a standalone wrapper for the ext2 root inode. This keeps ext2_find's
// return contract uniform: every successful lookup returns a heap-owned node.
static struct Node* ext2_open_root_dir(struct Ext2* fs){
  struct Inode* inode = icache_get(&fs->icache, fs->root.inumber);
  struct Node* root = malloc(sizeof(struct Node));

  node_init(root, fs->root.inumber, inode, fs);
  assert(node_is_dir(root), "ext2_open_root_dir: root inode is not a directory.\n");

  return root;
}

struct Node* ext2_enter_dir(struct Ext2* fs, struct Node* dir, struct RingBuf* path){
  char* name = ringbuf_remove_back(path);
  assert(name != NULL, "ext2_enter_dir: path is empty.\n");

  // get first directory entry
  unsigned index = 0;
  struct DirEntry entry;

  // traverse the linked list
  while (index < node_size_in_bytes(dir)) {
    int cnt = node_read_all(dir, index, sizeof(struct DirEntry), (char*)&entry);
    assert(cnt >= 4, "ext2_enter_dir: failed to read directory entry.\n");
    assert(entry.rec_len >= 8, "ext2_enter_dir: invalid directory record length.\n");

    index += entry.rec_len;
    if (strneq((char*)entry.name, name, entry.name_len) && entry.name_len == strlen(name) && entry.inode != 0) {

      assert(entry.inode != 0, "ext2_enter_dir: inode is 0.\n");

      // read inode table
      struct Inode* inode = icache_get(&fs->icache, entry.inode);
              
      free(name);
      struct Node* node = malloc(sizeof(struct Node));
      node_init(node, entry.inode, inode, fs);
      node->parent_inumber = dir->inumber;
      
      return node;
    }
  }

  free(name);
  return NULL;
}

struct Node* ext2_expand_symlink(struct Ext2* fs, struct Node* parent, struct Node* dir, struct RingBuf* path){
  unsigned target_size = node_size_in_bytes(dir);
  char* buf = malloc(target_size + 1);
  node_get_symlink_target(dir, buf);

  // Push the symlink target ahead of the remaining unresolved path so traversal
  // consumes the target before the old suffix.
  assert(ringbuf_add_back(path, buf), "ext2_expand_symlink: path buffer overflow.\n");

  if (buf[0] == '/') {
    return ext2_open_root_dir(fs);
  } else {
    // When the traversal starts from a symlink node, `parent == dir` and the
    // containing directory is not otherwise available. Reconstruct it so the
    // relative target uses the same base directory a normal path walk would use.
    if (parent == dir) {
      return ext2_open_parent_dir(dir);
    }

    return parent;
  }
}

// Releases heap-owned traversal wrappers while leaving borrowed nodes untouched.
static void ext2_release_owned_node(struct Node* node, bool owned){
  if (owned && node != NULL) {
    node_free(node);
  }
}

struct Node* ext2_find(struct Ext2* fs, struct Node* dir, char* name){
  if (dir == NULL || name == NULL) return NULL;
  assert(node_is_dir(dir) || node_is_symlink(dir), "ext2_find: not a directory or symlink.\n");

  bool dir_owned = false;

  // Absolute paths and root-relative paths start from a fresh heap-owned root
  // wrapper so all successful lookups have the same ownership contract.
  if (name[0] == '/' || dir == &fs->root) {
    dir = ext2_open_root_dir(fs);
    dir_owned = true;
  }


  struct Node* parent = dir;
  bool parent_owned = dir_owned;

  // surely path won't contain more than 1024 directories/simlinks to traverse
  struct RingBuf* path = malloc(sizeof(struct RingBuf));
  ringbuf_init(path, 1024);

  ext2_expand_path(fs, name, path);

  unsigned follows = 0;

  while (ringbuf_size(path) > 0) {
    if (dir == NULL) {
      ext2_free_pending_path(path);
      ext2_release_owned_node(parent, parent_owned);
      return NULL;
    }
    if (follows > 100) {
      ext2_free_pending_path(path);
      if (parent != dir) {
        ext2_release_owned_node(parent, parent_owned);
      }
      ext2_release_owned_node(dir, dir_owned);
      return NULL;
    }

    if (node_is_dir(dir)) {
      struct Node* old_parent = parent;
      bool old_parent_owned = parent_owned;
      struct Node* next = ext2_enter_dir(fs, dir, path);

      parent = dir;
      parent_owned = dir_owned;
      dir = next;
      dir_owned = next != NULL;

      if (old_parent != parent) {
        ext2_release_owned_node(old_parent, old_parent_owned);
      }
    } else {
      struct Node* old_parent = parent;
      bool old_parent_owned = parent_owned;
      struct Node* old_dir = dir;
      bool old_dir_owned = dir_owned;
      struct Node* tmp = ext2_expand_symlink(fs, old_parent, old_dir, path);

      follows += 1;

      if (tmp == old_parent) {
        dir = tmp;
        dir_owned = old_parent_owned;
      } else {
        dir = tmp;
        dir_owned = true;
      }

      // After expanding a symlink we restart traversal from `dir`, which is the
      // containing directory for relative targets or root for absolute targets.
      parent = dir;
      parent_owned = dir_owned;

      if (old_dir != dir) {
        ext2_release_owned_node(old_dir, old_dir_owned);
      }
      if (old_parent != old_dir && old_parent != dir) {
        ext2_release_owned_node(old_parent, old_parent_owned);
      }
    }
  }

  ext2_free_pending_path(path);

  if (parent != dir) {
    ext2_release_owned_node(parent, parent_owned);
  }

  return dir;
}


void icache_init(struct InodeCache* cache){
  blocking_lock_init(&cache->lock);
  for (unsigned i = 0; i < ICACHE_SIZE; ++i){
    // Clear tags so a freshly initialized cache cannot report a stale hit.
    cache->tags[i] = 0;
    cache->ages[i] = i;
  }
}

struct Inode* icache_get(struct InodeCache* cache, unsigned inumber){
  struct Inode* inode = malloc(sizeof(struct Inode));

  unsigned block_size = ext2_get_block_size(cache->fs);
  unsigned inode_size = ext2_get_inode_size(cache->fs);
  unsigned inodes_per_block = block_size / inode_size;

  blocking_lock_acquire(&cache->lock);
  bool found = false;
  unsigned result = 0;
  for (unsigned i = 0; i < ICACHE_SIZE; ++i){
    if (inumber == cache->tags[i]){
      found = true;
      result = i;
    }
  }

  if (found){
    for (unsigned i = 0; i < ICACHE_SIZE; ++i){
      if (cache->ages[i] < cache->ages[result]){
        cache->ages[i]++;
      }
    }
    cache->ages[result] = 0;

    // copy from buffer into inode
    memcpy(inode, cache->inode_cache + result, sizeof(struct Inode));

    blocking_lock_release(&cache->lock);

    return inode;
  } 
  
  
  // have to read in inode, so temporarily release lock while doing IO

  blocking_lock_release(&cache->lock);

  // Allocate the temporary inode-table buffer only on a cache miss so hot-path
  // inode hits do not contend on the heap for an unused scratch block.
  char* inode_table_buf = malloc(block_size);

  int rc = sd_read_blocks(SD_DRIVE_1,
    (cache->fs->bgd_table[(inumber - 1) / cache->fs->superblock.inodes_per_group].inode_table + 
      (inumber - 1) / inodes_per_block) * block_size / SD_SECTOR_SIZE_BYTES,
    block_size / SD_SECTOR_SIZE_BYTES, inode_table_buf
  );
  assert(rc == 0, "icache_get: failed to read inode table block.\n");

  blocking_lock_acquire(&cache->lock);

  for (unsigned i = 0; i < ICACHE_SIZE; ++i){
    cache->ages[i]++;
    if (cache->ages[i] == ICACHE_SIZE) result = i;
  }
    
  cache->ages[result] = 0;
  cache->tags[result] = inumber;

  // copy from buffer into cache
  memcpy(cache->inode_cache + result, (struct Inode*)(inode_table_buf + inode_size * ((inumber - 1) % inodes_per_block)),
    sizeof(struct Inode));

  // copy from cache into inode
  memcpy(inode, cache->inode_cache + result, sizeof(struct Inode));

  blocking_lock_release(&cache->lock);

  free(inode_table_buf);

  return inode;
}


void bcache_init(struct BlockCache* cache, unsigned block_size){
  blocking_lock_init(&cache->lock);
  cache->block_size = block_size;
  for (unsigned i = 0; i < BCACHE_SIZE; ++i){
    // Clear tags so a freshly initialized cache cannot report a stale hit.
    cache->tags[i] = 0;
    cache->ages[i] = i;
  }
}

void bcache_get(struct BlockCache* cache, unsigned block_num, char* dest){
  blocking_lock_acquire(&cache->lock);
  bool found = false;
  unsigned result = 0;
  for (unsigned i = 0; i < BCACHE_SIZE; ++i){
    if (block_num == cache->tags[i]){
      found = true;
      result = i;
    }
  }

  if (found){
    for (unsigned i = 0; i < BCACHE_SIZE; ++i){
      if (cache->ages[i] < cache->ages[result]){
        cache->ages[i]++;
      }
    }
    cache->ages[result] = 0;

    // copy from buffer into inode
    memcpy(dest, cache->block_cache + result * cache->block_size, cache->block_size);

    blocking_lock_release(&cache->lock);

    return;
  } 

  // was not in cache, need to read in block

  // can't read from sd while holding the lock since it's a blocking call
  blocking_lock_release(&cache->lock);

  int rc = sd_read_blocks(SD_DRIVE_1, block_num * cache->block_size / SD_SECTOR_SIZE_BYTES,
    cache->block_size / SD_SECTOR_SIZE_BYTES, dest);
  assert(rc == 0, "bcache_get: failed to read filesystem block.\n");

  // now update cache with new block
  blocking_lock_acquire(&cache->lock);
  for (unsigned i = 0; i < BCACHE_SIZE; ++i){
    cache->ages[i]++;
    if (cache->ages[i] == BCACHE_SIZE) result = i;
  }
  cache->ages[result] = 0;
  cache->tags[result] = block_num;

  memcpy(cache->block_cache + result * cache->block_size, dest, cache->block_size);
      
  blocking_lock_release(&cache->lock);
} 


void node_init(struct Node* node, unsigned inumber, struct Inode* inode, struct Ext2* fs){
  node->inumber = inumber;
  // Default to "self" so root nodes and reconstructed directory nodes remain
  // valid even when the caller has no better parent information available.
  node->parent_inumber = inumber;
  node->inode = inode;
  node->filesystem = fs;
}

void node_destroy(struct Node* node){
  free(node->inode);
}

void node_free(struct Node* node){
  node_destroy(node);
  free(node);
}

unsigned node_size_in_bytes(struct Node* node){
  return node->inode->size;
}

void read_sectors(struct Ext2* fs, unsigned index, char* buffer){
  bcache_get(&fs->bcache, index, buffer);
}

void read_direct_block(struct Node* node, unsigned index, char* buffer){
  assert(index < 12, "read_direct_block: index out of bounds for direct block.\n");

  read_sectors(node->filesystem, node->inode->block[index], buffer);
}

void read_indirect_block(struct Node* node, unsigned index, char* buffer){
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned entries_per_block = block_size / 4;
  assert(index >= 12, "read_indirect_block: index out of bounds for indirect block.\n");
  assert(index < 12 + entries_per_block, "read_indirect_block: index out of bounds for indirect block.\n");

  unsigned real_index = index - 12;

  unsigned* direct_pointers = malloc(block_size);
  read_sectors(node->filesystem, node->inode->block[12], (char*)direct_pointers);
  read_sectors(node->filesystem, direct_pointers[real_index], buffer);
  free(direct_pointers);
}

void read_double_indirect_block(struct Node* node, unsigned index, char* buffer){
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned entries_per_block = block_size / 4;
  assert(index >= 12 + entries_per_block, "read_double_indirect_block: index out of bounds for double indirect block.\n");
  assert(index < 12 + entries_per_block * (1 + entries_per_block), "read_double_indirect_block: index out of bounds for double indirect block.\n");

  unsigned real_index = index - (12 + entries_per_block);

  unsigned* indirect_pointers = malloc(block_size);
  unsigned* direct_pointers = malloc(block_size);
  read_sectors(node->filesystem, node->inode->block[13], (char*)indirect_pointers);
  read_sectors(node->filesystem, indirect_pointers[real_index / entries_per_block], (char*)direct_pointers);
  read_sectors(node->filesystem, direct_pointers[real_index % entries_per_block], buffer);
  free(indirect_pointers);
  free(direct_pointers);
}

void read_triple_indirect_block(struct Node* node, unsigned index, char* buffer){
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned entries_per_block = block_size / 4;
  assert(index >= 12 + entries_per_block * (1 + entries_per_block), "read_triple_indirect_block: index out of bounds for triple indirect block.\n");
  assert(index < 12 + entries_per_block * (1 + entries_per_block * (1 + entries_per_block)), "read_triple_indirect_block: index out of bounds for triple indirect block.\n");

  unsigned real_index = index - (12 + entries_per_block * (1 + entries_per_block));

  unsigned* double_indirect_pointers = malloc(block_size);
  unsigned* indirect_pointers = malloc(block_size);
  unsigned* direct_pointers = malloc(block_size);
    
  read_sectors(node->filesystem, node->inode->block[14], (char*)double_indirect_pointers);
  read_sectors(node->filesystem, double_indirect_pointers[real_index / (entries_per_block * entries_per_block)], (char*)indirect_pointers);
  read_sectors(node->filesystem, indirect_pointers[(real_index / entries_per_block) % entries_per_block], (char*)direct_pointers);
  read_sectors(node->filesystem, direct_pointers[real_index % entries_per_block], buffer);
  
  free(double_indirect_pointers);
  free(indirect_pointers);
  free(direct_pointers);
}

void node_read_block(struct Node* node, unsigned block_num, char* dest){
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned entries_per_block = block_size / 4;

  if (block_num < 12) read_direct_block(node, block_num, dest);
  else if (block_num < 12 + entries_per_block) read_indirect_block(node, block_num, dest);
  else if (block_num < 12 + entries_per_block * (1 + entries_per_block)) read_double_indirect_block(node, block_num, dest);
  else if (block_num < 12 + entries_per_block * (1 + entries_per_block * (1 + entries_per_block))){
    read_triple_indirect_block(node, block_num, dest);
  } else panic("invalid index for inode");
}

unsigned node_read_all(struct Node* node, unsigned offset, unsigned size, char* dest){
  if (size == 0) return 0;

  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned start_block = offset / block_size;
  unsigned end_block = (offset + size - 1) / block_size;
  unsigned bytes_copied = 0;

  for (unsigned i = start_block; i <= end_block; ++i){
    char* block_buf = malloc(block_size);
    node_read_block(node, i, block_buf);

    unsigned block_offset = (i == start_block) ? offset % block_size : 0;
    unsigned copy_size = (i == end_block) ? ((offset + size - 1) % block_size) - block_offset + 1 : block_size - block_offset;

    memcpy(dest + bytes_copied, block_buf + block_offset, copy_size);
    bytes_copied += copy_size;
    free(block_buf);
  }

  return size;
}

unsigned short node_get_type(struct Node* node){
  return node->inode->mode & 0xF000;
}

bool node_is_dir(struct Node* node){
  unsigned short type = node->inode->mode & 0xF000;
  return (type == EXT2_S_IFDIR);
}

bool node_is_file(struct Node* node){
  unsigned short type = node->inode->mode & 0xF000;
  return (type == EXT2_S_IFREG);
}

bool node_is_symlink(struct Node* node){
  unsigned short type = node->inode->mode & 0xF000;
  return (type == EXT2_S_IFLNK);
}

void node_get_symlink_target(struct Node* node, char* dest){
  assert(node_is_symlink(node), "node_get_symlink_target: node is not a symlink.\n");

  if (node->inode->size < 60) {
    memcpy(dest, &node->inode->block, node->inode->size);
    *(dest + node->inode->size) = 0;
  } else {
    node_read_all(node, 0, node->inode->size, dest);
    *(dest + node->inode->size) = 0;
  }
}

unsigned node_get_num_links(struct Node* node){
  return node->inode->links_count;
}

unsigned node_entry_count(struct Node* node){
  assert(node_is_dir(node), "node_entry_count: node is not a directory.\n");

  // get first directory entry
  unsigned index = 0;
  struct DirEntry entry;
  unsigned count = 0;

  // traverse the linked list
  while (index < node_size_in_bytes(node)){
    node_read_all(node, index, sizeof(struct DirEntry), (char*)&entry);
    assert(entry.rec_len >= 8, "node_entry_count: invalid directory record length.\n");
    index += entry.rec_len;
    if (entry.inode != 0) count++;
  }

  return count;
}
