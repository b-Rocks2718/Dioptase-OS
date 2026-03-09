#include "ext.h"
#include "sd_driver.h"
#include "print.h"
#include "debug.h"
#include "heap.h"
#include "string.h"

#define EXT2_SUPERBLOCK_SECTOR 2
#define EXT2_SUPERBLOCK_SECTORS 2
#define EXT2_DIR_ENTRY_HEADER_SIZE 8
#define EXT2_DIR_ENTRY_ALIGN_SIZE 4

// EXT2_S_IFREG | EXT2_S_IRUSR | EXT2_S_IWUSR | EXT2_S_IRGRP | EXT2_S_IROTH
#define EXT2_DEFAULT_FILE_MODE 0x81A4

// EXT2_S_IFDIR | EXT2_S_IRUSR | EXT2_S_IWUSR | EXT2_S_IXUSR | 
// EXT2_S_IRGRP | EXT2_S_IXGRP | EXT2_S_IROTH | EXT2_S_IXOTH
#define EXT2_DEFAULT_DIR_MODE 0x41ED

// EXT2_S_IFLNK | EXT2_S_IRUSR | EXT2_S_IWUSR | EXT2_S_IXUSR | 
// EXT2_S_IRGRP | EXT2_S_IWGRP | EXT2_S_IXGRP | EXT2_S_IROTH | 
// EXT2_S_IWOTH | EXT2_S_IXOTH
#define EXT2_DEFAULT_SYMLINK_MODE 0xA1FF

// ext2 stores directory records on 4-byte boundaries so readers can walk a
// block by following rec_len until the block boundary.
static unsigned ext2_dir_entry_min_size(unsigned name_len){
  unsigned size = EXT2_DIR_ENTRY_HEADER_SIZE + name_len;
  unsigned remainder = size % EXT2_DIR_ENTRY_ALIGN_SIZE;
  if (remainder != 0){
    size += EXT2_DIR_ENTRY_ALIGN_SIZE - remainder;
  }
  return size;
}

// The ext2 directory writer emits rev-0 entries: inode, rec_len, name_len, and
// the raw name bytes. Callers choose rec_len so the enclosing block remains a
// valid record chain all the way to its end.
static void ext2_write_dir_entry(char* dest, unsigned rec_len, char* name, unsigned inumber){
  unsigned name_len = strlen(name);

  assert(name_len <= sizeof(((struct DirEntry*)0)->name),
    "ext2_write_dir_entry: name is too long for ext2.\n");
  assert(rec_len >= ext2_dir_entry_min_size(name_len),
    "ext2_write_dir_entry: rec_len is too small for the directory entry.\n");

  struct DirEntry* entry = (struct DirEntry*)dest;
  entry->inode = inumber;
  entry->rec_len = rec_len;
  entry->name_len = name_len;
  strncpy((char*)entry->name, name, name_len);
}

// ext2 reserves 512-byte sectors in i_blocks, not filesystem blocks.
static unsigned ext2_sectors_per_block(struct Ext2* fs){
  return ext2_get_block_size(fs) / SD_SECTOR_SIZE_BYTES;
}

// The allocator mutates free counts in the primary superblock, so callers must
// flush it after every successful block or inode allocation.
static void ext2_write_superblock(struct Ext2* fs){
  int rc = sd_write_blocks(SD_DRIVE_1, EXT2_SUPERBLOCK_SECTOR,
    EXT2_SUPERBLOCK_SECTORS, (char*)&fs->superblock);
  assert(rc == 0, "ext2_write_superblock: failed to write ext2 superblock.\n");
}

// The block-group descriptor table can span a partial sector. The SD layer only
// writes whole sectors, so round the table size up before writing it back.
static void ext2_write_bgd_table(struct Ext2* fs){
  unsigned bgd_table_bytes = fs->num_block_groups * sizeof(struct BGD);
  unsigned bgd_table_sectors = (bgd_table_bytes + SD_SECTOR_SIZE_BYTES - 1) / SD_SECTOR_SIZE_BYTES;
  int rc = sd_write_blocks(SD_DRIVE_1, fs->bgd_offset / SD_SECTOR_SIZE_BYTES,
    bgd_table_sectors, (char*)fs->bgd_table);
  assert(rc == 0, "ext2_write_bgd_table: failed to write block group descriptor table.\n");
}

// Inode writeback is explicit in this filesystem implementation. Any helper
// that mutates on-disk inode fields must call this after the final state is set.
static void node_sync_inode(struct Node* node){
  icache_set(&node->filesystem->icache, node->inumber, node->inode);
}

// Directory sizes are tracked in bytes, while node_add_block needs the count of
// logical data blocks already present. ext2 i_blocks cannot be used for that
// because it is measured in 512-byte sectors and also includes indirect blocks.
static unsigned node_data_block_count(struct Node* node){
  unsigned block_size = ext2_get_block_size(node->filesystem);
  if (node->inode->size == 0){
    return 0;
  }

  return (node->inode->size - 1) / block_size + 1;
}

void ext2_init(struct Ext2* fs){
  // start by reading superblock
  int rc = sd_read_blocks(SD_DRIVE_1, 2, 2, &fs->superblock);
  assert(rc == 0, "ext2_init: failed to read ext2 superblock.\n");

  // check this is an ext2 file system
  assert(fs->superblock.magic == 0xEF53, "Not an ext2 file system\n");
  
  // read in the block group descriptor table
  unsigned block_size = ext2_get_block_size(fs);

  if (block_size >= 2048){
    fs->bgd_offset = block_size;
  } else {
    fs->bgd_offset = 2048;
  }

  // Assumes there is at least 1 block
  fs->num_block_groups = (fs->superblock.blocks_count - 1) / fs->superblock.blocks_per_group + 1;

  unsigned bgd_table_bytes = fs->num_block_groups * sizeof(struct BGD);
  unsigned bgd_table_sectors = (bgd_table_bytes + SD_SECTOR_SIZE_BYTES - 1) / SD_SECTOR_SIZE_BYTES;
  // The SD layer reads complete sectors, so the backing buffer must be rounded
  // up even if the descriptor table itself is smaller.
  fs->bgd_table = malloc(bgd_table_sectors * SD_SECTOR_SIZE_BYTES);

  // read in bgd table
  rc = sd_read_blocks(SD_DRIVE_1, fs->bgd_offset / SD_SECTOR_SIZE_BYTES,
    bgd_table_sectors, (char*)fs->bgd_table);
  assert(rc == 0, "ext2_init: failed to read block group descriptor table.\n");

  // read inode bitmaps
  fs->inode_bitmaps = malloc(fs->num_block_groups * sizeof(char*));
  for (unsigned i = 0; i < fs->num_block_groups; ++i){
    fs->inode_bitmaps[i] = malloc(block_size);
    rc = sd_read_blocks(SD_DRIVE_1, fs->bgd_table[i].inode_bitmap * block_size / SD_SECTOR_SIZE_BYTES,
      block_size / SD_SECTOR_SIZE_BYTES, fs->inode_bitmaps[i]);
    assert(rc == 0, "ext2_init: failed to read inode bitmap.\n");
  }

  // read in block bitmaps
  fs->block_bitmaps = malloc(fs->num_block_groups * sizeof(char*));
  for (unsigned i = 0; i < fs->num_block_groups; ++i){
    fs->block_bitmaps[i] = malloc(block_size);
    rc = sd_read_blocks(SD_DRIVE_1, fs->bgd_table[i].block_bitmap * block_size / SD_SECTOR_SIZE_BYTES,
      block_size / SD_SECTOR_SIZE_BYTES, fs->block_bitmaps[i]);
    assert(rc == 0, "ext2_init: failed to read block bitmap.\n");
  }

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

  for (unsigned i = 0; i < fs->num_block_groups; ++i){
    free(fs->inode_bitmaps[i]);
    free(fs->block_bitmaps[i]);
  }
  free(fs->inode_bitmaps);
  free(fs->block_bitmaps);
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

  // iterate over dir entries
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

unsigned alloc_inumber(struct Ext2* fs){
  unsigned inumber = 0;

  // find block group with free inodes
  for (unsigned i = 0; i < fs->num_block_groups; ++i){
    if (fs->bgd_table[i].free_inodes_count > 0){
      // found block group
      fs->bgd_table[i].free_inodes_count -= 1;
      fs->superblock.free_inodes_count -= 1;
      
      // find free inode in bitmap
      for (int j = 0; j < fs->superblock.inodes_per_group / 8; ++j){
        if (fs->inode_bitmaps[i][j] != 0xFF){
          // found free inode
          for (int k = 0; k < 8; ++k){
            if ((fs->inode_bitmaps[i][j] & (1 << k)) == 0){
              fs->inode_bitmaps[i][j] |= (1 << k); // mark as used
              inumber = i * fs->superblock.inodes_per_group + j * 8 + k + 1; // calculate inumber
              break;
            }
          }
        }
        if (inumber != 0) break;
      }

      // write back updated bitmap and bgd
      int rc = sd_write_blocks(SD_DRIVE_1, fs->bgd_table[i].inode_bitmap * ext2_get_block_size(fs) / SD_SECTOR_SIZE_BYTES,
        ext2_get_block_size(fs) / SD_SECTOR_SIZE_BYTES, fs->inode_bitmaps[i]);
      assert(rc == 0, "alloc_inumber: failed to write back inode bitmap.\n");

      ext2_write_bgd_table(fs);
      ext2_write_superblock(fs);
      
      break;
    }
  }

  return inumber;
}

unsigned alloc_block(struct Ext2* fs){
  unsigned block_num = -1;

  // find block group with free inodes
  for (unsigned i = 0; i < fs->num_block_groups; ++i){
    if (fs->bgd_table[i].free_blocks_count > 0){
      // found block group
      fs->bgd_table[i].free_blocks_count -= 1;
      fs->superblock.free_blocks_count -= 1;
      
      // find free block in bitmap
      for (int j = 0; j < fs->superblock.blocks_per_group / 8; ++j){
        if (fs->block_bitmaps[i][j] != 0xFF){
          // found free block
          for (int k = 0; k < 8; ++k){
            if ((fs->block_bitmaps[i][j] & (1 << k)) == 0){
              fs->block_bitmaps[i][j] |= (1 << k); // mark as used
              block_num = i * fs->superblock.blocks_per_group + j * 8 + k; // calculate block number
              break;
            }
          }
        }
        
        if (block_num != -1) break;
      }

      // write back updated bitmap and bgd
      int rc = sd_write_blocks(SD_DRIVE_1, fs->bgd_table[i].block_bitmap * ext2_get_block_size(fs) / SD_SECTOR_SIZE_BYTES,
        ext2_get_block_size(fs) / SD_SECTOR_SIZE_BYTES, fs->block_bitmaps[i]);
      assert(rc == 0, "alloc_block: failed to write back block bitmap.\n");

      ext2_write_bgd_table(fs);
      ext2_write_superblock(fs);

      break;
    }
  }

  return block_num;
}

struct Inode* make_inode(short mode){
  struct Inode* inode = malloc(sizeof(struct Inode));

  inode->mode = mode;
  inode->uid = 0; // TODO: support non-root users
  inode->size = 0; // new file has no data blocks
  inode->atime = 0; // not supported
  inode->ctime = 0; // not supported
  inode->mtime = 0; // not supported
  inode->dtime = 0; // not supported
  inode->gid = 0; // TODO: support non-root groups
  inode->links_count = 1; // new file has one link from its parent directory
  inode->blocks = 0; // no data blocks yet
  inode->flags = 0; // not supported
  inode->osd1 = 0; // not used
  
  for (int i = 0; i < 15; ++i){
    inode->block[i] = 0; // no data blocks yet
  }

  inode->generation = 0; // not supported
  inode->dir_acl = 0; // rev 0 requires this to be 0
  inode->faddr = 0; // not supported

  for (int i = 0; i < 12; ++i){
    inode->osd2[i] = 0; // not used
  }

  return inode;
}

bool node_add_block(struct Node* node, unsigned block_num){
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned sectors_per_block = ext2_sectors_per_block(node->filesystem);
  unsigned entries_per_block = block_size / 4;
  unsigned single_limit = 12 + entries_per_block;
  unsigned double_span = entries_per_block * entries_per_block;
  unsigned double_limit = single_limit + double_span;
  unsigned triple_span = double_span * entries_per_block;
  unsigned triple_limit = double_limit + triple_span;
  unsigned logical_block = node_data_block_count(node);
  unsigned reserved_blocks = 1;

  if (logical_block < 12){
    node->inode->block[logical_block] = block_num;
    node->inode->blocks += reserved_blocks * sectors_per_block;
    return true;
  } else if (logical_block < single_limit){
    unsigned* single_indirect = malloc(block_size);
    if (logical_block == 12){
      unsigned new_block = alloc_block(node->filesystem);
      if (new_block == -1){
        free(single_indirect);
        return false;
      }
      node->inode->block[12] = new_block;
      reserved_blocks += 1;
    } else {
      bcache_get(&node->filesystem->bcache, node->inode->block[12], (char*)single_indirect);
    }

    single_indirect[logical_block - 12] = block_num;
    bcache_set(&node->filesystem->bcache, node->inode->block[12], (char*)single_indirect, 0, block_size);
    node->inode->blocks += reserved_blocks * sectors_per_block;
    free(single_indirect);
    return true;
  } else if (logical_block < double_limit){
    unsigned double_index = logical_block - single_limit;
    unsigned indirect_index = double_index / entries_per_block;
    unsigned direct_index = double_index % entries_per_block;
    unsigned* double_indirect = malloc(block_size);
    unsigned* single_indirect = malloc(block_size);

    if (logical_block == single_limit){
      unsigned new_double_block = alloc_block(node->filesystem);
      if (new_double_block == -1){
        free(double_indirect);
        free(single_indirect);
        return false;
      }
      node->inode->block[13] = new_double_block;
      reserved_blocks += 1;
    } else {
      bcache_get(&node->filesystem->bcache, node->inode->block[13], (char*)double_indirect);
    }

    if (direct_index == 0){
      unsigned new_block = alloc_block(node->filesystem);
      if (new_block == -1){
        free(double_indirect);
        free(single_indirect);
        return false;
      }

      double_indirect[indirect_index] = new_block;
      reserved_blocks += 1;
      bcache_set(&node->filesystem->bcache, node->inode->block[13], (char*)double_indirect, 0, block_size);
    } else {
      bcache_get(&node->filesystem->bcache, double_indirect[indirect_index], (char*)single_indirect);
    }

    single_indirect[direct_index] = block_num;
    bcache_set(&node->filesystem->bcache, double_indirect[indirect_index], (char*)single_indirect, 0, block_size);
    node->inode->blocks += reserved_blocks * sectors_per_block;
    free(double_indirect);
    free(single_indirect);
    return true;
  } else if (logical_block < triple_limit){
    unsigned triple_index = logical_block - double_limit;
    unsigned outer_index = triple_index / double_span;
    unsigned middle_index = (triple_index / entries_per_block) % entries_per_block;
    unsigned direct_index = triple_index % entries_per_block;
    unsigned* triple_indirect = malloc(block_size);
    unsigned* double_indirect = malloc(block_size);
    unsigned* single_indirect = malloc(block_size);

    if (logical_block == double_limit){
      unsigned new_triple_block = alloc_block(node->filesystem);
      if (new_triple_block == -1){
        free(triple_indirect);
        free(double_indirect);
        free(single_indirect);
        return false;
      }
      node->inode->block[14] = new_triple_block;
      reserved_blocks += 1;
    } else {
      bcache_get(&node->filesystem->bcache, node->inode->block[14], (char*)triple_indirect);
    }

    if (triple_index % double_span == 0){
      unsigned new_block = alloc_block(node->filesystem);
      if (new_block == -1){
        free(triple_indirect);
        free(double_indirect);
        free(single_indirect);
        return false;
      }

      triple_indirect[outer_index] = new_block;
      reserved_blocks += 1;
      bcache_set(&node->filesystem->bcache, node->inode->block[14], (char*)triple_indirect, 0, block_size);
    } else {
      bcache_get(&node->filesystem->bcache, triple_indirect[outer_index], (char*)double_indirect);
    }

    if (direct_index == 0){
      unsigned new_block = alloc_block(node->filesystem);
      if (new_block == -1){
        free(triple_indirect);
        free(double_indirect);
        free(single_indirect);
        return false;
      }

      double_indirect[middle_index] = new_block;
      reserved_blocks += 1;
      bcache_set(&node->filesystem->bcache, triple_indirect[outer_index], (char*)double_indirect, 0, block_size);
    } else {
      bcache_get(&node->filesystem->bcache, double_indirect[middle_index], (char*)single_indirect);
    }

    single_indirect[direct_index] = block_num;
    bcache_set(&node->filesystem->bcache, double_indirect[middle_index], (char*)single_indirect, 0, block_size);
    node->inode->blocks += reserved_blocks * sectors_per_block;
    free(triple_indirect);
    free(double_indirect);
    free(single_indirect);
    return true;
  } else {
    return false;
  }
}

// Search one directory data block for slack in an existing record. ext2 grows a
// directory by splitting the last record that has spare rec_len bytes; only a
// completely full directory needs a new data block.
static bool dir_insert_entry_in_existing_block(struct Node* dir, unsigned block_index, char* name, unsigned inumber){
  unsigned block_size = ext2_get_block_size(dir->filesystem);
  unsigned new_entry_size = ext2_dir_entry_min_size(strlen(name));
  char* block_buf = malloc(block_size);

  node_read_block(dir, block_index, block_buf);

  unsigned offset = 0;
  while (offset < block_size){
    struct DirEntry* existing = (struct DirEntry*)(block_buf + offset);
    unsigned record_length = existing->rec_len;

    assert(record_length >= EXT2_DIR_ENTRY_HEADER_SIZE,
      "dir_insert_entry_in_existing_block: invalid directory record length.\n");
    assert(record_length % EXT2_DIR_ENTRY_ALIGN_SIZE == 0,
      "dir_insert_entry_in_existing_block: directory record is not 4-byte aligned.\n");
    assert(offset + record_length <= block_size,
      "dir_insert_entry_in_existing_block: directory record crosses the block boundary.\n");

    if (existing->inode == 0){
      if (record_length >= new_entry_size){
        ext2_write_dir_entry(block_buf + offset, record_length, name, inumber);
        node_write_block(dir, block_index, block_buf, 0, block_size);
        free(block_buf);
        return true;
      }
    } else {
      unsigned ideal_length = ext2_dir_entry_min_size(existing->name_len);
      if (record_length >= ideal_length + new_entry_size){
        existing->rec_len = ideal_length;
        ext2_write_dir_entry(block_buf + offset + ideal_length,
          record_length - ideal_length, name, inumber);
        node_write_block(dir, block_index, block_buf, 0, block_size);
        free(block_buf);
        return true;
      }
    }

    offset += record_length;
  }

  assert(offset == block_size,
    "dir_insert_entry_in_existing_block: directory block did not terminate at the block boundary.\n");

  free(block_buf);
  return false;
}

void dir_add_entry(struct Node* dir, char* name, unsigned inumber){
  unsigned block_size = ext2_get_block_size(dir->filesystem);
  unsigned logical_block_count = node_data_block_count(dir);

  assert(node_is_dir(dir), "dir_add_entry: target node is not a directory.\n");
  assert(strlen(name) > 0, "dir_add_entry: directory entries must have a non-empty name.\n");

  for (unsigned i = 0; i < logical_block_count; ++i){
    if (dir_insert_entry_in_existing_block(dir, i, name, inumber)){
      return;
    }
  }

  // No existing record had enough slack, so the directory must grow by one full
  // data block. The new block starts with one record that owns the entire block.
  unsigned new_block = alloc_block(dir->filesystem);
  assert(new_block != (unsigned)-1, "dir_add_entry: failed to allocate a new directory block.\n");

  bool added = node_add_block(dir, new_block);
  assert(added, "dir_add_entry: failed to attach the new directory block to the inode.\n");

  char* block_buf = malloc(block_size);
  ext2_write_dir_entry(block_buf, block_size, name, inumber);
  node_write_block(dir, logical_block_count, block_buf, 0, block_size);
  free(block_buf);

  dir->inode->size += block_size;
  node_sync_inode(dir);
}

// Duplicate names are rejected before allocation so failed creates do not
// consume inode numbers or mutate the parent directory on disk.
static bool dir_has_entry_name(struct Node* dir, char* name){
  unsigned index = 0;
  unsigned name_len = strlen(name);
  struct DirEntry entry;

  assert(node_is_dir(dir), "dir_has_entry_name: target node is not a directory.\n");
  assert(name != NULL, "dir_has_entry_name: name is NULL.\n");

  while (index < node_size_in_bytes(dir)){
    int cnt = node_read_all(dir, index, sizeof(struct DirEntry), (char*)&entry);
    assert(cnt >= 4, "dir_has_entry_name: failed to read directory entry.\n");
    assert(entry.rec_len >= 8, "dir_has_entry_name: invalid directory record length.\n");

    index += entry.rec_len;
    if (entry.inode != 0 && entry.name_len == name_len &&
        strneq((char*)entry.name, name, name_len)) {
      return true;
    }
  }

  return false;
}

struct Node* alloc_inode(struct Ext2* fs, struct Node* dir, char* name, short mode){
  assert(dir != NULL, "alloc_inode: parent directory is NULL.\n");
  assert(name != NULL, "alloc_inode: name is NULL.\n");

  if (dir_has_entry_name(dir, name)){
    return NULL;
  }

  unsigned inumber = alloc_inumber(fs);
  if (inumber == 0){
    return NULL;
  }

  struct Inode* inode = make_inode(mode);
  struct Node* node = malloc(sizeof(struct Node));
  node_init(node, inumber, inode, fs);

  // inode allocated successfully, now update parent directory
  dir_add_entry(dir, name, inumber);

  // write new inode to disk
  node_sync_inode(node);

  return node;
}

struct Node* ext2_make_file(struct Ext2* fs, struct Node* dir, char* name){
  // New regular files default to owner-writable, world-readable mode so the
  // extracted host artifact is readable without an extra chmod step.
  struct Node* node = alloc_inode(fs, dir, name, EXT2_DEFAULT_FILE_MODE);
  if (node == NULL){
    return NULL;
  }

  return node;
}

struct Node* ext2_make_dir(struct Ext2* fs, struct Node* dir, char* name){
  // New directories default to executable/traversable permissions for all
  // readers while remaining writable only by the owner.
  struct Node* node = alloc_inode(fs, dir, name, EXT2_DEFAULT_DIR_MODE);

  if (node == NULL){
    return NULL;
  }

  // add . and .. entries to new directory
  dir_add_entry(node, ".", node->inumber);
  dir_add_entry(node, "..", dir->inumber);

  return node;
}

struct Node* ext2_make_symlink(struct Ext2* fs, struct Node* dir, char* name, char* target){
  // Symlinks traditionally carry 0777 permissions even though most hosts ignore
  // them when dereferencing the link target.
  struct Node* node = alloc_inode(fs, dir, name, EXT2_DEFAULT_SYMLINK_MODE);

  if (node == NULL){
    return NULL;
  }

  // write symlink target
  node_write_all(node, 0, strlen(target), target);

  return node;
}

void icache_init(struct InodeCache* cache){
  blocking_lock_init(&cache->lock);
  for (unsigned i = 0; i < ICACHE_SIZE; ++i){
    // Clear tags so a freshly initialized cache cannot report a stale hit.
    cache->tags[i] = -1;
    cache->ages[i] = i;
  }
}

struct Inode* icache_get(struct InodeCache* cache, unsigned inumber){
  struct Inode* inode = malloc(sizeof(struct Inode));

  unsigned block_size = ext2_get_block_size(cache->fs);
  unsigned inode_size = ext2_get_inode_size(cache->fs);
  unsigned inodes_per_block = block_size / inode_size;
  unsigned block_group = (inumber - 1) / cache->fs->superblock.inodes_per_group;
  unsigned block_group_inode_index = (inumber - 1) % cache->fs->superblock.inodes_per_group;
  unsigned inode_table_sector =
    (cache->fs->bgd_table[block_group].inode_table + block_group_inode_index / inodes_per_block) *
    block_size / SD_SECTOR_SIZE_BYTES;
  unsigned inode_offset = inode_size * (block_group_inode_index % inodes_per_block);

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

  int rc = sd_read_blocks(SD_DRIVE_1, inode_table_sector,
    block_size / SD_SECTOR_SIZE_BYTES, inode_table_buf);
  assert(rc == 0, "icache_get: failed to read inode table block.\n");

  blocking_lock_acquire(&cache->lock);

  for (unsigned i = 0; i < ICACHE_SIZE; ++i){
    cache->ages[i]++;
    if (cache->ages[i] == ICACHE_SIZE) result = i;
  }
    
  cache->ages[result] = 0;
  cache->tags[result] = inumber;

  // copy from buffer into cache
  memcpy(cache->inode_cache + result, (struct Inode*)(inode_table_buf + inode_offset), sizeof(struct Inode));

  // copy from cache into inode
  memcpy(inode, cache->inode_cache + result, sizeof(struct Inode));

  blocking_lock_release(&cache->lock);

  free(inode_table_buf);

  return inode;
}

void icache_set(struct InodeCache* cache, unsigned inumber, struct Inode* inode){
  unsigned block_size = ext2_get_block_size(cache->fs);
  unsigned inode_size = ext2_get_inode_size(cache->fs);
  unsigned inodes_per_block = block_size / inode_size;
  unsigned block_group = (inumber - 1) / cache->fs->superblock.inodes_per_group;
  unsigned block_group_inode_index = (inumber - 1) % cache->fs->superblock.inodes_per_group;
  unsigned inode_table_sector =
    (cache->fs->bgd_table[block_group].inode_table + block_group_inode_index / inodes_per_block) *
    block_size / SD_SECTOR_SIZE_BYTES;
  unsigned inode_offset = inode_size * (block_group_inode_index % inodes_per_block);
  char* inode_table_buf = malloc(block_size);

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
  } else {
    for (unsigned i = 0; i < ICACHE_SIZE; ++i){
      cache->ages[i]++;
      if (cache->ages[i] == ICACHE_SIZE) result = i;
    }
    cache->tags[result] = inumber;
  }

  cache->ages[result] = 0;
  memcpy(cache->inode_cache + result, inode, sizeof(struct Inode));
  blocking_lock_release(&cache->lock);

  // Read-modify-write the containing inode-table block so neighboring inodes in
  // the same block are preserved, then write it back after the cache update.
  int rc = sd_read_blocks(SD_DRIVE_1, inode_table_sector,
    block_size / SD_SECTOR_SIZE_BYTES, inode_table_buf);
  assert(rc == 0, "icache_set: failed to read inode table block.\n");

  memcpy(inode_table_buf + inode_offset, inode, sizeof(struct Inode));

  rc = sd_write_blocks(SD_DRIVE_1, inode_table_sector,
    block_size / SD_SECTOR_SIZE_BYTES, inode_table_buf);
  assert(rc == 0, "icache_set: failed to write inode table block.\n");

  free(inode_table_buf);
}

void bcache_init(struct BlockCache* cache, unsigned block_size){
  blocking_lock_init(&cache->lock);
  cache->block_size = block_size;
  for (unsigned i = 0; i < BCACHE_SIZE; ++i){
    // Clear tags so a freshly initialized cache cannot report a stale hit.
    cache->tags[i] = -1;
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

void bcache_set(struct BlockCache* cache, unsigned block_num, char* src, unsigned offset, unsigned size){
  char* block_buf = malloc(cache->block_size);

  blocking_lock_acquire(&cache->lock);
  bool found = false;
  unsigned result = 0;
  for (unsigned i = 0; i < BCACHE_SIZE; ++i){
    if (block_num == cache->tags[i]){
      found = true;
      result = i;
    }
  }

  if (found || (size >= cache->block_size && offset == 0)){
    // Cache hits already have a complete block image in memory. Full-block
    // overwrites also do not need a read-before-write path because every byte
    // in the block will be replaced by src.
    if (found){
      for (unsigned i = 0; i < BCACHE_SIZE; ++i){
        if (cache->ages[i] < cache->ages[result]){
          cache->ages[i]++;
        }
      }
    } else {
      for (unsigned i = 0; i < BCACHE_SIZE; ++i){
        cache->ages[i]++;
        if (cache->ages[i] == BCACHE_SIZE) result = i;
      }
    }

    cache->ages[result] = 0;
    cache->tags[result] = block_num;

    unsigned write_size = size >= (cache->block_size - offset) ? (cache->block_size - offset) : size;
    memcpy(cache->block_cache + result * cache->block_size + offset, src, write_size);
    memcpy(block_buf, cache->block_cache + result * cache->block_size, cache->block_size);

    blocking_lock_release(&cache->lock);

    // write back to sd
    int rc = sd_write_blocks(SD_DRIVE_1, block_num * cache->block_size / SD_SECTOR_SIZE_BYTES,
      cache->block_size / SD_SECTOR_SIZE_BYTES, block_buf);
    assert(rc == 0, "bcache_set: failed to write filesystem block.\n");

    free(block_buf);

    return;
  }

  blocking_lock_release(&cache->lock);

  // need to read block first so we can do a partial write without overwriting the rest of the block
  int rc = sd_read_blocks(SD_DRIVE_1, block_num * cache->block_size / SD_SECTOR_SIZE_BYTES,
    cache->block_size / SD_SECTOR_SIZE_BYTES, block_buf);
  assert(rc == 0, "bcache_set: failed to read filesystem block before a partial write.\n");

  blocking_lock_acquire(&cache->lock);
  for (unsigned i = 0; i < BCACHE_SIZE; ++i){
    cache->ages[i]++;
    if (cache->ages[i] == BCACHE_SIZE) result = i;
  }
  cache->ages[result] = 0;
  cache->tags[result] = block_num;

  unsigned write_size = size >= (cache->block_size - offset) ? (cache->block_size - offset) : size;

  memcpy(cache->block_cache + result * cache->block_size, block_buf, cache->block_size);  
  memcpy(cache->block_cache + result * cache->block_size + offset, src, write_size);
  memcpy(block_buf + offset, src, write_size);
  blocking_lock_release(&cache->lock);

  // write to disk after releasing lock since it's a blocking call and we don't want to hold
  // the lock during it
  rc = sd_write_blocks(SD_DRIVE_1, block_num * cache->block_size / SD_SECTOR_SIZE_BYTES,
    cache->block_size / SD_SECTOR_SIZE_BYTES, block_buf);
  assert(rc == 0, "bcache_set: failed to write filesystem block.\n");

  free(block_buf);
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

void node_print_dir(struct Node* node){
  unsigned index = 0;
  struct DirEntry entry;
  
  while (index < node_size_in_bytes(node)) {
    int cnt = node_read_all(node, index, sizeof(struct DirEntry), (char*)&entry);
    assert(cnt >= 4, "node_print_dir: failed to read directory entry.\n");
    assert(entry.rec_len >= 8, "node_print_dir: invalid directory record length.\n");
  
    index += entry.rec_len;
    if (entry.inode != 0) {
      // copy name into buf
      char* name_buf = malloc(entry.name_len + 1);
      strncpy(name_buf, (char*)entry.name, entry.name_len);
      name_buf[entry.name_len] = '\0';
      printf("%s\n", &name_buf);
      free(name_buf);
    }
  }
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
  
void write_sectors(struct Ext2* fs, unsigned index, char* buffer, unsigned offset, unsigned size){
  bcache_set(&fs->bcache, index, buffer, offset, size);
}

void write_direct_block(struct Node* node, unsigned index, char* buffer, unsigned offset, unsigned size){
  assert(index < 12, "write_direct_block: index out of bounds for direct block.\n");

  write_sectors(node->filesystem, node->inode->block[index], buffer, offset, size);
}

void write_indirect_block(struct Node* node, unsigned index, char* buffer, unsigned offset, unsigned size){
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned entries_per_block = block_size / 4;
  assert(index >= 12, "write_indirect_block: index out of bounds for indirect block.\n");
  assert(index < 12 + entries_per_block, "write_indirect_block: index out of bounds for indirect block.\n");

  unsigned real_index = index - 12;

  unsigned* direct_pointers = malloc(block_size);
  read_sectors(node->filesystem, node->inode->block[12], (char*)direct_pointers);
  write_sectors(node->filesystem, direct_pointers[real_index], buffer, offset, size);
  free(direct_pointers);
}

void write_double_indirect_block(struct Node* node, unsigned index, char* buffer, unsigned offset, unsigned size){
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned entries_per_block = block_size / 4;
  assert(index >= 12 + entries_per_block, "read_double_indirect_block: index out of bounds for double indirect block.\n");
  assert(index < 12 + entries_per_block * (1 + entries_per_block), "read_double_indirect_block: index out of bounds for double indirect block.\n");

  unsigned real_index = index - (12 + entries_per_block);

  unsigned* indirect_pointers = malloc(block_size);
  unsigned* direct_pointers = malloc(block_size);
  read_sectors(node->filesystem, node->inode->block[13], (char*)indirect_pointers);
  read_sectors(node->filesystem, indirect_pointers[real_index / entries_per_block], (char*)direct_pointers);
  write_sectors(node->filesystem, direct_pointers[real_index % entries_per_block], buffer, offset, size);
  free(indirect_pointers);
  free(direct_pointers);
}

void write_triple_indirect_block(struct Node* node, unsigned index, char* buffer, unsigned offset, unsigned size){
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned entries_per_block = block_size / 4;
  assert(index >= 12 + entries_per_block * (1 + entries_per_block), "write_triple_indirect_block: index out of bounds for triple indirect block.\n");
  assert(index < 12 + entries_per_block * (1 + entries_per_block * (1 + entries_per_block)), "write_triple_indirect_block: index out of bounds for triple indirect block.\n");

  unsigned real_index = index - (12 + entries_per_block * (1 + entries_per_block));

  unsigned* double_indirect_pointers = malloc(block_size);
  unsigned* indirect_pointers = malloc(block_size);
  unsigned* direct_pointers = malloc(block_size);
    
  read_sectors(node->filesystem, node->inode->block[14], (char*)double_indirect_pointers);
  read_sectors(node->filesystem, double_indirect_pointers[real_index / (entries_per_block * entries_per_block)], (char*)indirect_pointers);
  read_sectors(node->filesystem, indirect_pointers[(real_index / entries_per_block) % entries_per_block], (char*)direct_pointers);
  write_sectors(node->filesystem, direct_pointers[real_index % entries_per_block], buffer, offset, size);
  
  free(double_indirect_pointers);
  free(indirect_pointers);
  free(direct_pointers);
}

void node_write_block(struct Node* node, unsigned block_num, char* src, unsigned offset, unsigned size){
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned entries_per_block = block_size / 4;

  if (block_num < 12) write_direct_block(node, block_num, src, offset, size);
  else if (block_num < 12 + entries_per_block) write_indirect_block(node, block_num, src, offset, size);
  else if (block_num < 12 + entries_per_block * (1 + entries_per_block)) write_double_indirect_block(node, block_num, src, offset, size);
  else if (block_num < 12 + entries_per_block * (1 + entries_per_block * (1 + entries_per_block))){
    write_triple_indirect_block(node, block_num, src, offset, size);
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

unsigned node_write_all(struct Node* node, unsigned offset, unsigned size, char* src){
  if (size == 0) return 0;

  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned start_block = offset / block_size;
  unsigned end_block = (offset + size - 1) / block_size;
  unsigned bytes_copied = 0;

  for (unsigned i = start_block; i <= end_block; ++i){
    unsigned block_offset = (i == start_block) ? offset % block_size : 0;
    unsigned copy_size = (i == end_block) ? ((offset + size - 1) % block_size) - block_offset + 1 : block_size - block_offset;
    node_write_block(node, i, src + bytes_copied, block_offset, copy_size);

    bytes_copied += copy_size;
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
