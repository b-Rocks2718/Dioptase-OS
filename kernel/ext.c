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

static void cached_inode_init(struct CachedInode* cached, unsigned inumber);

// ext2 block and inode bitmaps can end mid-byte when the per-group count is not
// divisible by 8, so scans must round up to cover the partial final byte.
static unsigned ext2_bitmap_bytes(unsigned entries_per_group){
  return (entries_per_group + 7) / 8;
}

// Absolute ext2 block numbers start at first_data_block for the filesystem.
static unsigned ext2_block_group_start(struct Ext2* fs, unsigned group_index){
  return fs->superblock.first_data_block + group_index * fs->superblock.blocks_per_group;
}

static unsigned ext2_block_group_index(struct Ext2* fs, unsigned block_num){
  assert(block_num >= fs->superblock.first_data_block,
    "ext2_block_group_index: block number is before first_data_block.\n");
  return (block_num - fs->superblock.first_data_block) / fs->superblock.blocks_per_group;
}

static unsigned ext2_block_local_index(struct Ext2* fs, unsigned block_num){
  assert(block_num >= fs->superblock.first_data_block,
    "ext2_block_local_index: block number is before first_data_block.\n");
  return (block_num - fs->superblock.first_data_block) % fs->superblock.blocks_per_group;
}

static bool ext2_name_is_dot(char* name){
  return name[0] == '.' && name[1] == '\0';
}

static bool ext2_name_is_dot_dot(char* name){
  return name[0] == '.' && name[1] == '.' && name[2] == '\0';
}

static bool ext2_name_has_separator(char* name){
  unsigned name_len = strlen(name);

  for (unsigned i = 0; i < name_len; ++i){
    if (name[i] == '/') {
      return true;
    }
  }

  return false;
}

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
  icache_set(&node->filesystem->icache, node->cached);
}

// Count contiguous non-zero block pointers in one indirect block. Ext2 files in
// this implementation only grow by appending blocks, so the first zero entry
// terminates the logical data-block span.
static unsigned ext2_count_indirect_entries(unsigned* block, unsigned entries_per_block){
  unsigned count = 0;

  while (count < entries_per_block && block[count] != 0){
    count += 1;
  }

  return count;
}

// Clear a newly allocated pointer block before publishing it. That guarantees
// later scans stop at the first unused entry instead of reading heap garbage as
// block numbers.
static void ext2_zero_pointer_block(unsigned* block, unsigned entries_per_block){
  memset(block, 0, entries_per_block * sizeof(unsigned));
}

// Directory sizes are tracked in bytes, while node_add_block needs the count of
// logical data blocks already attached to the inode. ext2 i_blocks cannot be
// used for that because it counts 512-byte sectors for both file data and
// metadata blocks such as indirect pointer blocks.
static unsigned node_scan_data_block_count(struct Node* node){
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned entries_per_block = block_size / 4;
  unsigned count = 0;
  unsigned single_count = 0;
  unsigned* single_indirect = NULL;
  unsigned* double_indirect = NULL;
  unsigned* triple_indirect = NULL;

  while (count < 12 && node->cached->inode.block[count] != 0){
    count += 1;
  }

  if (count < 12 || node->cached->inode.block[12] == 0){
    return count;
  }

  single_indirect = malloc(block_size);
  bcache_get(&node->filesystem->bcache, node->cached->inode.block[12], (char*)single_indirect);
  single_count = ext2_count_indirect_entries(single_indirect, entries_per_block);
  count += single_count;
  if (single_count < entries_per_block || node->cached->inode.block[13] == 0){
    free(single_indirect);
    return count;
  }

  double_indirect = malloc(block_size);
  bcache_get(&node->filesystem->bcache, node->cached->inode.block[13], (char*)double_indirect);
  for (unsigned outer = 0; outer < entries_per_block; ++outer){
    if (double_indirect[outer] == 0){
      free(double_indirect);
      free(single_indirect);
      return count;
    }

    bcache_get(&node->filesystem->bcache, double_indirect[outer], (char*)single_indirect);
    single_count = ext2_count_indirect_entries(single_indirect, entries_per_block);
    count += single_count;
    if (single_count < entries_per_block){
      free(double_indirect);
      free(single_indirect);
      return count;
    }
  }

  if (node->cached->inode.block[14] == 0){
    free(double_indirect);
    free(single_indirect);
    return count;
  }

  triple_indirect = malloc(block_size);
  bcache_get(&node->filesystem->bcache, node->cached->inode.block[14], (char*)triple_indirect);
  for (unsigned outer = 0; outer < entries_per_block; ++outer){
    if (triple_indirect[outer] == 0){
      free(triple_indirect);
      free(double_indirect);
      free(single_indirect);
      return count;
    }

    bcache_get(&node->filesystem->bcache, triple_indirect[outer], (char*)double_indirect);
    for (unsigned middle = 0; middle < entries_per_block; ++middle){
      if (double_indirect[middle] == 0){
        free(triple_indirect);
        free(double_indirect);
        free(single_indirect);
        return count;
      }

      bcache_get(&node->filesystem->bcache, double_indirect[middle], (char*)single_indirect);
      single_count = ext2_count_indirect_entries(single_indirect, entries_per_block);
      count += single_count;
      if (single_count < entries_per_block){
        free(triple_indirect);
        free(double_indirect);
        free(single_indirect);
        return count;
      }
    }
  }

  free(triple_indirect);
  free(double_indirect);
  free(single_indirect);
  return count;
}

void ext2_init(struct Ext2* fs){
  // start by reading superblock
  int rc = sd_read_blocks(SD_DRIVE_1, 2, 2, &fs->superblock);
  assert(rc == 0, "ext2_init: failed to read ext2 superblock.\n");

  // check this is an ext2 file system
  assert(fs->superblock.magic == 0xEF53,
    "ext2_init: filesystem superblock magic does not identify an ext2 image.\n");
  
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

  blocking_lock_init(&fs->metadata_lock);
  blocking_lock_init(&fs->inode_lock);

  // Read the root inode through the inode cache so bootstrap uses the same
  // inode size and block-size rules as every other inode lookup.
  struct CachedInode* root_inode = icache_get(&fs->icache, EXT2_ROOT_INO);

  // The ext2 root inode must always be a directory.
  assert((root_inode->inode.mode & 0xF000) == EXT2_S_IFDIR,
    "ext2_init: root inode is not a directory.\n");

  node_init(&fs->root, root_inode, EXT2_BAD_INO, fs);
}

void ext2_destroy(struct Ext2* fs){
  free(fs->bgd_table);
  node_destroy(&fs->root);

  for (unsigned i = 0; i < fs->num_block_groups; ++i){
    free(fs->inode_bitmaps[i]);
    free(fs->block_bitmaps[i]);
  }
  free(fs->inode_bitmaps);
  free(fs->block_bitmaps);

  icache_destroy(&fs->icache);
  blocking_lock_destroy(&fs->metadata_lock);
  blocking_lock_destroy(&fs->inode_lock);
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

// Insert a slash-separated path string ahead of the remaining unresolved path
// components in `path`. `path` already stores the caller's suffix in
// ringbuf_remove_back order, so rebuild the queue as:
//   target components, then the old suffix components.
static void ext2_prepend_path(struct Ext2* fs, struct RingBuf* path, char* target){
  unsigned suffix_size = ringbuf_size(path);
  struct RingBuf rebuilt;

  ringbuf_init(&rebuilt, path->capacity);
  ext2_expand_path(fs, target, &rebuilt);

  for (unsigned i = 0; i < suffix_size; ++i){
    char* component = ringbuf_remove_back(path);
    assert(component != NULL, "ext2_prepend_path: path queue unexpectedly underflowed.\n");
    assert(ringbuf_add_front(&rebuilt, component), "ext2_prepend_path: path buffer overflow.\n");
  }

  ringbuf_destroy(path);
  *path = rebuilt;
}

// Releases any path components still owned by the traversal queue, then frees
// the queue object itself. `node_find` uses this on both success and failure so
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
  struct CachedInode* cached = icache_get(&fs->icache, node->parent_inumber);
  struct Node* parent = malloc(sizeof(struct Node));

  node_init(parent, cached, EXT2_BAD_INO, fs);
  assert(node_is_dir(parent),
    "ext2_open_parent_dir: symlink parent inode is not a directory.\n");

  return parent;
}

// Create a standalone wrapper for the ext2 root inode. This keeps node_find's
// return contract uniform: every successful lookup returns a heap-owned node.
static struct Node* ext2_open_root_dir(struct Ext2* fs){
  struct CachedInode* cached = icache_get(&fs->icache, fs->root.cached->inumber);
  struct Node* root = malloc(sizeof(struct Node));

  node_init(root, cached, EXT2_BAD_INO, fs);
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
      struct CachedInode* inode = icache_get(&fs->icache, entry.inode);
              
      free(name);
      struct Node* node = malloc(sizeof(struct Node));
      node_init(node, inode, dir->cached->inumber, fs);
      
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

  // The symlink target is itself a path string, not one literal directory name.
  // Rebuild the traversal queue so slash-separated target components are
  // prepended ahead of the still-unresolved suffix from the original lookup.
  ext2_prepend_path(fs, path, buf);

  if (buf[0] == '/') {
    free(buf);
    return ext2_open_root_dir(fs);
  } else {
    // When the traversal starts from a symlink node, `parent == dir` and the
    // containing directory is not otherwise available. Reconstruct it so the
    // relative target uses the same base directory a normal path walk would use.
    if (parent == dir) {
      free(buf);
      return ext2_open_parent_dir(dir);
    }

    free(buf);
    return parent;
  }
}

// Releases heap-owned traversal wrappers while leaving borrowed nodes untouched.
static void ext2_release_owned_node(struct Node* node, bool owned){
  if (owned && node != NULL) {
    node_free(node);
  }
}

struct Node* node_find(struct Node* dir, char* name){
  if (dir == NULL || name == NULL) return NULL;
  assert(node_is_dir(dir) || node_is_symlink(dir), "node_find: not a directory or symlink.\n");

  bool dir_owned = false;

  // Absolute paths and root-relative paths start from a fresh heap-owned root
  // wrapper so all successful lookups have the same ownership contract.
  if (name[0] == '/' || dir == &dir->filesystem->root) {
    dir = ext2_open_root_dir(dir->filesystem);
    dir_owned = true;
  }


  struct Node* parent = dir;
  bool parent_owned = dir_owned;

  // surely path won't contain more than 1024 directories/simlinks to traverse
  struct RingBuf* path = malloc(sizeof(struct RingBuf));
  ringbuf_init(path, 1024);

  ext2_expand_path(dir->filesystem, name, path);

  if (ringbuf_size(path) == 0){
    struct Node* result = dir;

    ext2_free_pending_path(path);

    if (!dir_owned){
      struct CachedInode* cached = icache_get(&dir->filesystem->icache, dir->cached->inumber);
      result = malloc(sizeof(struct Node));
      node_init(result, cached, dir->parent_inumber, dir->filesystem);
    }

    return result;
  }

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
      struct Node* next = ext2_enter_dir(dir->filesystem, dir, path);

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
      struct Node* tmp = ext2_expand_symlink(dir->filesystem, old_parent, old_dir, path);

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

unsigned alloc_inumber(struct Ext2* fs, short mode){
  unsigned inumber = 0;
  unsigned bitmap_bytes = ext2_bitmap_bytes(fs->superblock.inodes_per_group);

  blocking_lock_acquire(&fs->metadata_lock);

  // find block group with free inodes
  for (unsigned i = 0; i < fs->num_block_groups; ++i){
    if (fs->bgd_table[i].free_inodes_count > 0){
      // found block group
      fs->bgd_table[i].free_inodes_count -= 1;
      fs->superblock.free_inodes_count -= 1;
      
      // find free inode in bitmap
      for (unsigned j = 0; j < bitmap_bytes; ++j){
        if (fs->inode_bitmaps[i][j] != 0xFF){
          // found free inode
          for (unsigned k = 0; k < 8; ++k){
            unsigned local_index = j * 8 + k;

            if (local_index >= fs->superblock.inodes_per_group){
              break;
            }

            if ((fs->inode_bitmaps[i][j] & (1 << k)) == 0){
              fs->inode_bitmaps[i][j] |= (1 << k); // mark as used
              inumber = i * fs->superblock.inodes_per_group + local_index + 1; // calculate inumber
              break;
            }
          }
        }
        if (inumber != 0) break;
      }

      if ((mode & 0xF000) == EXT2_S_IFDIR){
        fs->bgd_table[i].used_dirs_count += 1;
      }

      // for now, double-locking seems necessary to avoid the case of
      // an old superblock accidentally being written back after a newer one
      // TODO: refactor to avoid this
      ext2_write_bgd_table(fs);
      ext2_write_superblock(fs);

      // write back updated bitmap
      int rc = sd_write_blocks(SD_DRIVE_1, fs->bgd_table[i].inode_bitmap * ext2_get_block_size(fs) / SD_SECTOR_SIZE_BYTES,
        ext2_get_block_size(fs) / SD_SECTOR_SIZE_BYTES, fs->inode_bitmaps[i]);
      assert(rc == 0, "alloc_inumber: failed to write back inode bitmap.\n");

      blocking_lock_release(&fs->metadata_lock);
      
      break;
    }
  }

  if (inumber == 0){
    panic("alloc_inumber: no free inodes available.\n");
  }

  return inumber;
}

void dealloc_inumber(struct Ext2* fs, unsigned inumber, short mode) {
  blocking_lock_acquire(&fs->metadata_lock);

  // find block group containing inumber
  unsigned group_index = (inumber - 1) / fs->superblock.inodes_per_group;
  unsigned local_index = (inumber - 1) % fs->superblock.inodes_per_group;
  unsigned byte_index = local_index / 8;
  unsigned bit_index = local_index % 8;

  fs->inode_bitmaps[group_index][byte_index] &= ~(1 << bit_index); // mark as free
  fs->bgd_table[group_index].free_inodes_count += 1;
  fs->superblock.free_inodes_count += 1;

  if ((mode & 0xF000) == EXT2_S_IFDIR){
    fs->bgd_table[group_index].used_dirs_count -= 1;
  }

  // for now, double-locking seems necessary to avoid the case of
  // an old superblock accidentally being written back after a newer one
  // TODO: refactor to avoid this
  ext2_write_bgd_table(fs);
  ext2_write_superblock(fs);

  // write back updated bitmap
  int rc = sd_write_blocks(SD_DRIVE_1, fs->bgd_table[group_index].inode_bitmap * ext2_get_block_size(fs) / SD_SECTOR_SIZE_BYTES,
    ext2_get_block_size(fs) / SD_SECTOR_SIZE_BYTES, fs->inode_bitmaps[group_index]);
  assert(rc == 0, "alloc_inumber: failed to write back inode bitmap.\n");

  blocking_lock_release(&fs->metadata_lock);
}

unsigned alloc_block(struct Ext2* fs){
  unsigned block_num = -1;
  unsigned bitmap_bytes = ext2_bitmap_bytes(fs->superblock.blocks_per_group);

  blocking_lock_acquire(&fs->metadata_lock);

  // find block group with free inodes
  for (unsigned i = 0; i < fs->num_block_groups; ++i){
    if (fs->bgd_table[i].free_blocks_count > 0){
      // found block group
      fs->bgd_table[i].free_blocks_count -= 1;
      fs->superblock.free_blocks_count -= 1;
      
      // find free block in bitmap
      for (unsigned j = 0; j < bitmap_bytes; ++j){
        if (fs->block_bitmaps[i][j] != 0xFF){
          // found free block
          for (unsigned k = 0; k < 8; ++k){
            unsigned local_index = j * 8 + k;

            if (local_index >= fs->superblock.blocks_per_group){
              break;
            }

            if ((fs->block_bitmaps[i][j] & (1 << k)) == 0){
              fs->block_bitmaps[i][j] |= (1 << k); // mark as used
              block_num = ext2_block_group_start(fs, i) + local_index;
              break;
            }
          }
        }
        
        if (block_num != -1) break;
      }

      ext2_write_bgd_table(fs);
      ext2_write_superblock(fs);

      // write back updated bitmap
      int rc = sd_write_blocks(SD_DRIVE_1, fs->bgd_table[i].block_bitmap * ext2_get_block_size(fs) / SD_SECTOR_SIZE_BYTES,
        ext2_get_block_size(fs) / SD_SECTOR_SIZE_BYTES, fs->block_bitmaps[i]);
      assert(rc == 0, "alloc_block: failed to write back block bitmap.\n");

      // A reused block may still contain bytes from the inode that previously
      // owned it, both on disk and in the block cache. Zero it before returning
      // so later partial writes and gap reads observe a clean block image.
      char* zero_block = malloc(ext2_get_block_size(fs));
      memset(zero_block, 0, ext2_get_block_size(fs));
      bcache_set(&fs->bcache, block_num, zero_block, 0, ext2_get_block_size(fs));
      free(zero_block);

      blocking_lock_release(&fs->metadata_lock);

      break;
    }
  }

  if (block_num == -1){
    panic("alloc_block: no free blocks available.\n");
  }

  return block_num;
}

void dealloc_block(struct Ext2* fs, unsigned block_num) {
  blocking_lock_acquire(&fs->metadata_lock);

  // find block group containing block_num
  unsigned group_index = ext2_block_group_index(fs, block_num);
  unsigned local_index = ext2_block_local_index(fs, block_num);
  unsigned byte_index = local_index / 8;
  unsigned bit_index = local_index % 8;

  fs->block_bitmaps[group_index][byte_index] &= ~(1 << bit_index); // mark as free
  fs->bgd_table[group_index].free_blocks_count += 1;
  fs->superblock.free_blocks_count += 1;

  ext2_write_bgd_table(fs);
  ext2_write_superblock(fs);

  // write back updated bitmap
  int rc = sd_write_blocks(SD_DRIVE_1, fs->bgd_table[group_index].block_bitmap * ext2_get_block_size(fs) / SD_SECTOR_SIZE_BYTES,
    ext2_get_block_size(fs) / SD_SECTOR_SIZE_BYTES, fs->block_bitmaps[group_index]);
  assert(rc == 0, "dealloc_block: failed to write back block bitmap.\n");

  blocking_lock_release(&fs->metadata_lock);
}

// Release one indirect pointer block and every block reachable from it. `levels`
// is the number of pointer hops from this block to data blocks:
// 1 = single indirect, 2 = double indirect, 3 = triple indirect.
static void ext2_free_indirect_tree(struct Ext2* fs, unsigned pointer_block_num, unsigned levels){
  unsigned block_size = ext2_get_block_size(fs);
  unsigned entries_per_block = block_size / sizeof(unsigned);
  unsigned* pointers = malloc(block_size);

  assert(levels >= 1 && levels <= 3, "ext2_free_indirect_tree: invalid indirect level.\n");
  assert(pointer_block_num != 0, "ext2_free_indirect_tree: pointer block number is zero.\n");

  bcache_get(&fs->bcache, pointer_block_num, (char*)pointers);

  for (unsigned i = 0; i < entries_per_block; ++i){
    if (pointers[i] == 0) continue;

    if (levels == 1){
      dealloc_block(fs, pointers[i]);
    } else {
      ext2_free_indirect_tree(fs, pointers[i], levels - 1);
    }
  }

  free(pointers);
  dealloc_block(fs, pointer_block_num);
}

// Deleting the final name for an inode must reclaim every filesystem block that
// belongs to that inode. Fast symlinks are excluded because short targets are
// stored inline in inode.block[] instead of in allocatable filesystem blocks.
static void node_dealloc_blocks(struct Node* node){
  bool fast_symlink = (node->cached->inode.mode & 0xF000) == EXT2_S_IFLNK &&
    node->cached->inode.size <= sizeof(node->cached->inode.block);

  if (!fast_symlink){
    for (unsigned i = 0; i < 12; ++i){
      if (node->cached->inode.block[i] != 0){
        dealloc_block(node->filesystem, node->cached->inode.block[i]);
      }
    }

    if (node->cached->inode.block[12] != 0){
      ext2_free_indirect_tree(node->filesystem, node->cached->inode.block[12], 1);
    }

    if (node->cached->inode.block[13] != 0){
      ext2_free_indirect_tree(node->filesystem, node->cached->inode.block[13], 2);
    }

    if (node->cached->inode.block[14] != 0){
      ext2_free_indirect_tree(node->filesystem, node->cached->inode.block[14], 3);
    }
  }

  memset(node->cached->inode.block, 0, sizeof(node->cached->inode.block));
  node->cached->inode.blocks = 0;
  node->cached->inode.size = 0;
  node->cached->data_block_count = 0;
}

struct CachedInode* make_inode(short mode, unsigned inumber){
  struct CachedInode* cached = malloc(sizeof(struct CachedInode));
  cached_inode_init(cached, inumber);

  cached->inode.mode = mode;
  cached->inode.uid = 0; // TODO: support non-root users
  cached->inode.size = 0; // new file has no data blocks
  cached->inode.atime = 0; // not supported
  cached->inode.ctime = 0; // not supported
  cached->inode.mtime = 0; // not supported
  cached->inode.dtime = 0; // not supported
  cached->inode.gid = 0; // TODO: support non-root groups
  cached->inode.links_count = (mode & 0xF000) == EXT2_S_IFDIR ? 2 : 1; // directory has extra link for ".", file has 1 link
  cached->inode.blocks = 0; // no data blocks yet
  cached->inode.flags = 0; // not supported
  cached->inode.osd1 = 0; // not used
  memset(cached->inode.block, 0, sizeof(cached->inode.block));

  cached->inode.generation = 0; // not supported
  cached->inode.dir_acl = 0; // rev 0 requires this to be 0
  cached->inode.faddr = 0; // not supported
  memset(cached->inode.osd2, 0, sizeof(cached->inode.osd2));

  return cached;
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
  unsigned logical_block = node->cached->data_block_count;
  unsigned reserved_blocks = 1;

  if (logical_block < 12){
    node->cached->inode.block[logical_block] = block_num;
    node->cached->inode.blocks += reserved_blocks * sectors_per_block;
    node->cached->data_block_count += 1;
    return true;
  } else if (logical_block < single_limit){
    unsigned* single_indirect = malloc(block_size);
    if (logical_block == 12){
      unsigned new_block = alloc_block(node->filesystem);
      if (new_block == -1){
        free(single_indirect);
        return false;
      }
      node->cached->inode.block[12] = new_block;
      ext2_zero_pointer_block(single_indirect, entries_per_block);
      reserved_blocks += 1;
    } else {
      bcache_get(&node->filesystem->bcache, node->cached->inode.block[12], (char*)single_indirect);
    }

    single_indirect[logical_block - 12] = block_num;
    bcache_set(&node->filesystem->bcache, node->cached->inode.block[12], (char*)single_indirect, 0, block_size);
    node->cached->inode.blocks += reserved_blocks * sectors_per_block;
    node->cached->data_block_count += 1;
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
      node->cached->inode.block[13] = new_double_block;
      ext2_zero_pointer_block(double_indirect, entries_per_block);
      reserved_blocks += 1;
    } else {
      bcache_get(&node->filesystem->bcache, node->cached->inode.block[13], (char*)double_indirect);
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
      bcache_set(&node->filesystem->bcache, node->cached->inode.block[13], (char*)double_indirect, 0, block_size);
      ext2_zero_pointer_block(single_indirect, entries_per_block);
    } else {
      bcache_get(&node->filesystem->bcache, double_indirect[indirect_index], (char*)single_indirect);
    }

    single_indirect[direct_index] = block_num;
    bcache_set(&node->filesystem->bcache, double_indirect[indirect_index], (char*)single_indirect, 0, block_size);
    node->cached->inode.blocks += reserved_blocks * sectors_per_block;
    node->cached->data_block_count += 1;
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
      node->cached->inode.block[14] = new_triple_block;
      ext2_zero_pointer_block(triple_indirect, entries_per_block);
      reserved_blocks += 1;
    } else {
      bcache_get(&node->filesystem->bcache, node->cached->inode.block[14], (char*)triple_indirect);
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
      bcache_set(&node->filesystem->bcache, node->cached->inode.block[14], (char*)triple_indirect, 0, block_size);
      ext2_zero_pointer_block(double_indirect, entries_per_block);
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
      ext2_zero_pointer_block(single_indirect, entries_per_block);
    } else {
      bcache_get(&node->filesystem->bcache, double_indirect[middle_index], (char*)single_indirect);
    }

    single_indirect[direct_index] = block_num;
    bcache_set(&node->filesystem->bcache, double_indirect[middle_index], (char*)single_indirect, 0, block_size);
    node->cached->inode.blocks += reserved_blocks * sectors_per_block;
    node->cached->data_block_count += 1;
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
static bool dir_insert_entry_in_existing_block(struct Node* dir, unsigned block_index, char* name, unsigned inumber, bool has_lock){
  unsigned block_size = ext2_get_block_size(dir->filesystem);
  unsigned new_entry_size = ext2_dir_entry_min_size(strlen(name));
  char* block_buf = malloc(block_size);

  if (!has_lock){
    blocking_lock_acquire(&dir->cached->lock);
  }
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
        if (!has_lock){
          blocking_lock_release(&dir->cached->lock);
        }
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
        if (!has_lock){
          blocking_lock_release(&dir->cached->lock);
        }
        free(block_buf);
        return true;
      }
    }

    offset += record_length;
  }

  assert(offset == block_size,
    "dir_insert_entry_in_existing_block: directory block did not terminate at the block boundary.\n");
  if (!has_lock){
    blocking_lock_release(&dir->cached->lock);
  }
  free(block_buf);
  return false;
}

bool dir_add_entry(struct Node* dir, char* name, unsigned inumber, bool has_lock){
  unsigned block_size = ext2_get_block_size(dir->filesystem);
  unsigned logical_block_count = dir->cached->data_block_count;

  assert(node_is_dir(dir), "dir_add_entry: target node is not a directory.\n");
  assert(strlen(name) > 0, "dir_add_entry: directory entries must have a non-empty name.\n");

  for (unsigned i = 0; i < logical_block_count; ++i){
    if (dir_insert_entry_in_existing_block(dir, i, name, inumber, has_lock)){
      return true;
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

  dir->cached->inode.size += block_size;
  node_sync_inode(dir);

  return true;
}

bool dir_remove_entry(struct Node* dir, char* name, bool has_lock){
  unsigned block_size = ext2_get_block_size(dir->filesystem);
  unsigned logical_block_count = dir->cached->data_block_count;

  assert(node_is_dir(dir), "dir_remove_entry: target node is not a directory.\n");
  assert(name != NULL, "dir_remove_entry: name is NULL.\n");

  for (unsigned i = 0; i < logical_block_count; ++i){
    char* block_buf = malloc(block_size);

    if (!has_lock){
      blocking_lock_acquire(&dir->cached->lock);
    }
    node_read_block(dir, i, block_buf);

    unsigned offset = 0;
    struct DirEntry* prev = NULL;
    while (offset < block_size){
      struct DirEntry* existing = (struct DirEntry*)(block_buf + offset);
      unsigned record_length = existing->rec_len;

      assert(record_length >= EXT2_DIR_ENTRY_HEADER_SIZE,
        "dir_remove_entry: invalid directory record length.\n");
      assert(record_length % EXT2_DIR_ENTRY_ALIGN_SIZE == 0,
        "dir_remove_entry: directory record is not 4-byte aligned.\n");
      assert(offset + record_length <= block_size,
        "dir_remove_entry: directory record crosses the block boundary.\n");

      if (existing->inode != 0 && existing->name_len == strlen(name) &&
          strneq((char*)existing->name, name, existing->name_len)) {
        existing->inode = 0;
        if (prev != NULL){
          prev->rec_len += existing->rec_len;
        }
        node_write_block(dir, i, block_buf, 0, block_size);
        if (!has_lock){
          blocking_lock_release(&dir->cached->lock);
        }
        free(block_buf);

        return true;
      }
      

      offset += record_length;
      prev = existing;
    }

    assert(offset == block_size,
      "dir_remove_entry: directory block did not terminate at the block boundary.\n");

    if (!has_lock){
      blocking_lock_release(&dir->cached->lock);
    }
    free(block_buf);
  }

  return false;
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

// ext2 directories are empty when every live entry is either "." or "..".
// Removed entries with inode == 0 are free space and do not make the directory
// non-empty.
static bool dir_is_empty(struct Node* dir){
  unsigned index = 0;
  struct DirEntry entry;

  assert(node_is_dir(dir), "dir_is_empty: target node is not a directory.\n");

  blocking_lock_acquire(&dir->cached->lock);

  while (index < node_size_in_bytes(dir)){
    int cnt = node_read_all(dir, index, sizeof(struct DirEntry), (char*)&entry);
    assert(cnt >= 4, "dir_is_empty: failed to read directory entry.\n");
    assert(entry.rec_len >= EXT2_DIR_ENTRY_HEADER_SIZE,
      "dir_is_empty: invalid directory record length.\n");
    assert(entry.rec_len % EXT2_DIR_ENTRY_ALIGN_SIZE == 0,
      "dir_is_empty: directory record is not 4-byte aligned.\n");

    if (entry.inode != 0){
      bool is_dot = entry.name_len == 1 && strneq((char*)entry.name, ".", 1);
      bool is_dot_dot = entry.name_len == 2 && strneq((char*)entry.name, "..", 2);

      if (!is_dot && !is_dot_dot){
        blocking_lock_release(&dir->cached->lock);
        return false;
      }
    }

    index += entry.rec_len;
  }

  blocking_lock_release(&dir->cached->lock);
  return true;
}

struct Node* alloc_inode(struct Ext2* fs, struct Node* dir, char* name, short mode){
  assert(dir != NULL, "alloc_inode: parent directory is NULL.\n");
  assert(name != NULL, "alloc_inode: name is NULL.\n");
  assert(strlen(name) > 0, "alloc_inode: name is empty.\n");
  assert(!ext2_name_has_separator(name),
    "alloc_inode: names must be one directory entry component without '/'.\n");
  assert(!ext2_name_is_dot(name),
    "alloc_inode: '.' is reserved and cannot be created as a new directory entry.\n");
  assert(!ext2_name_is_dot_dot(name),
    "alloc_inode: '..' is reserved and cannot be created as a new directory entry.\n");

  // Serialize duplicate-name detection and insertion so two concurrent creates
  // of the same basename cannot both observe the name as free.
  blocking_lock_acquire(&dir->cached->lock);

  if (dir_has_entry_name(dir, name)){
    blocking_lock_release(&dir->cached->lock);
    return NULL;
  }

  unsigned inumber = alloc_inumber(fs, mode);
  if (inumber == 0){
    blocking_lock_release(&dir->cached->lock);
    return NULL;
  }

  struct CachedInode* cached = make_inode(mode, inumber);
  icache_insert(&fs->icache, cached);

  struct Node* node = malloc(sizeof(struct Node));

  node_init(node, cached, dir->cached->inumber, fs);

  // inode allocated successfully, now update parent directory
  bool added = dir_add_entry(dir, name, inumber, true);
  assert(added, "alloc_inode: failed to add the new directory entry to the parent directory.\n");

  if ((mode & 0xF000) == EXT2_S_IFDIR){
    // parent dir gets new link from child's .. entry
    dir->cached->inode.links_count += 1;
    node_sync_inode(dir);
  }

  // write new inode to disk
  node_sync_inode(node);

  blocking_lock_release(&dir->cached->lock);

  return node;
}

static void dealloc_inode(struct Node* node){
  assert(node->cached->inode.links_count == 1, "dealloc_inode: cannot delete a node with multiple links.\n");

  node_dealloc_blocks(node);
  dealloc_inumber(node->filesystem, node->cached->inumber, node->cached->inode.mode);
}

static void cached_inode_init(struct CachedInode* cached, unsigned inumber){
  cached->inumber = inumber;
  cached->refcount = 1;
  cached->valid = false; // invalid until the inode data is read in from disk
  blocking_lock_init(&cached->lock);
  gate_init(&cached->valid_gate);
}

static void cached_inode_destroy(struct CachedInode* cached){
  gate_destroy(&cached->valid_gate);
  blocking_lock_destroy(&cached->lock);
}

static void cached_inode_free(struct CachedInode* cached){
  cached_inode_destroy(cached);
  free(cached);
}

void icache_init(struct InodeCache* cache){
  blocking_lock_init(&cache->lock);
  hash_map_init(&cache->cache, 1024);
}

struct CachedInode* icache_get(struct InodeCache* cache, unsigned inumber){
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
  
  // check live cache
  struct CachedInode* cached = hash_map_get(&cache->cache, inumber);
  if (cached != NULL){
    // cache hit in live cache, can return immediately
    cached->refcount += 1;
    blocking_lock_release(&cache->lock);

    gate_wait(&cached->valid_gate);

    return cached;
  } 
  
  // have to read in inode, so temporarily release lock while doing IO
  blocking_lock_release(&cache->lock);

  // insert entry into cache before reading, so we avoid reading twice
  struct CachedInode* new_cache_entry = malloc(sizeof(struct CachedInode));
  cached_inode_init(new_cache_entry, inumber);

  blocking_lock_acquire(&cache->lock);
  struct CachedInode* old = hash_map_try_insert(&cache->cache, inumber, new_cache_entry);
  if (old != NULL){
    old->refcount += 1;
  }
  blocking_lock_release(&cache->lock);

  if (old != NULL){
    // this inode is already being read by another thread,
    // so we will wait for it to become valid and then return the same cache entry
    cached_inode_free(new_cache_entry); // wasn't needed

    gate_wait(&old->valid_gate);

    assert(old->valid, 
      "icache_get: gate was signaled but cache entry is not valid.\n");

    return old;
  }

  // Allocate the temporary inode-table buffer only on a cache miss so hot-path
  // inode hits do not contend on the heap for an unused scratch block.
  char* inode_table_buf = malloc(block_size);

  int rc = sd_read_blocks(SD_DRIVE_1, inode_table_sector,
    block_size / SD_SECTOR_SIZE_BYTES, inode_table_buf);
  assert(rc == 0, "icache_get: failed to read inode table block.\n");

  blocking_lock_acquire(&cache->lock);

  // copy from buffer into cache
  memcpy(&new_cache_entry->inode, (struct Inode*)(inode_table_buf + inode_offset), sizeof(struct Inode));

  new_cache_entry->valid = true;
  blocking_lock_release(&cache->lock);

  gate_signal(&new_cache_entry->valid_gate);

  free(inode_table_buf);

  return new_cache_entry;
}

void icache_insert(struct InodeCache* cache, struct CachedInode* cached){
  blocking_lock_acquire(&cache->lock);
  struct CachedInode* old = hash_map_try_insert(&cache->cache, cached->inumber, cached);
  assert(old == NULL, "icache_insert: attempted to insert duplicate cache entry.\n");
  blocking_lock_release(&cache->lock);

  gate_signal(&cached->valid_gate);
}

void icache_set(struct InodeCache* cache, struct CachedInode* cached){
  unsigned block_size = ext2_get_block_size(cache->fs);
  unsigned inode_size = ext2_get_inode_size(cache->fs);
  unsigned inodes_per_block = block_size / inode_size;
  unsigned block_group = (cached->inumber - 1) / cache->fs->superblock.inodes_per_group;
  unsigned block_group_inode_index = (cached->inumber - 1) % cache->fs->superblock.inodes_per_group;
  unsigned inode_table_sector =
    (cache->fs->bgd_table[block_group].inode_table + block_group_inode_index / inodes_per_block) *
    block_size / SD_SECTOR_SIZE_BYTES;
  unsigned inode_offset = inode_size * (block_group_inode_index % inodes_per_block);
  char* inode_table_buf = malloc(block_size);

  // Read-modify-write the containing inode-table block so neighboring inodes in
  // the same block are preserved. This needs to be atomic to avoid losing updated
  // if two threads change different inodes in the same block

  blocking_lock_acquire(&cache->fs->inode_lock);

  int rc = sd_read_blocks(SD_DRIVE_1, inode_table_sector,
    block_size / SD_SECTOR_SIZE_BYTES, inode_table_buf);
  assert(rc == 0, "icache_set: failed to read inode table block.\n");

  memcpy(inode_table_buf + inode_offset, &cached->inode, sizeof(struct Inode));

  rc = sd_write_blocks(SD_DRIVE_1, inode_table_sector,
    block_size / SD_SECTOR_SIZE_BYTES, inode_table_buf);
  assert(rc == 0, "icache_set: failed to write inode table block.\n");

  blocking_lock_release(&cache->fs->inode_lock);

  free(inode_table_buf);
}

// decrement refcount, free if it hits 0
// Note that the caller is responsible for writing back dirty cache entries before releasing them
void icache_release(struct InodeCache* cache, struct CachedInode* cached){
  blocking_lock_acquire(&cache->lock);

  assert(cached->refcount > 0, "icache_release: attempted to release an inode cache entry with refcount 0.\n");
  cached->refcount -= 1;

  if (cached->refcount == 0){
    hash_map_remove(&cache->cache, cached->inumber);

    blocking_lock_release(&cache->lock);

    cached_inode_free(cached);
  } else {
    blocking_lock_release(&cache->lock);
  }
}

void icache_destroy(struct InodeCache* cache){
  hash_map_destroy(&cache->cache);
  blocking_lock_destroy(&cache->lock);
}

void icache_free(struct InodeCache* cache){
  icache_destroy(cache);
  free(cache);
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

    // write back to sd
    int rc = sd_write_blocks(SD_DRIVE_1, block_num * cache->block_size / SD_SECTOR_SIZE_BYTES,
      cache->block_size / SD_SECTOR_SIZE_BYTES, block_buf);
    assert(rc == 0, "bcache_set: failed to write filesystem block.\n");

    blocking_lock_release(&cache->lock);

    free(block_buf);

    return;
  }

  // need to read block first so we can do a partial write without overwriting the rest of the block
  int rc = sd_read_blocks(SD_DRIVE_1, block_num * cache->block_size / SD_SECTOR_SIZE_BYTES,
    cache->block_size / SD_SECTOR_SIZE_BYTES, block_buf);
  assert(rc == 0, "bcache_set: failed to read filesystem block before a partial write.\n");

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

  rc = sd_write_blocks(SD_DRIVE_1, block_num * cache->block_size / SD_SECTOR_SIZE_BYTES,
    cache->block_size / SD_SECTOR_SIZE_BYTES, block_buf);
  assert(rc == 0, "bcache_set: failed to write filesystem block.\n");

  blocking_lock_release(&cache->lock);

  free(block_buf);
}

void node_init(struct Node* node, struct CachedInode* cached, unsigned parent_inumber, struct Ext2* fs){
  node->cached = cached;
  // Default to "self" so root nodes and reconstructed directory nodes remain
  // valid even when the caller has no better parent information available.
  node->parent_inumber = parent_inumber;
  node->filesystem = fs;
  // Seed the cache once when the wrapper is created. Later block-growth paths
  // update it incrementally so steady-state writes avoid pointer-tree scans.
  node->cached->data_block_count = node_scan_data_block_count(node);
}

void node_destroy(struct Node* node){
  icache_release(&node->filesystem->icache, node->cached);
}

void node_free(struct Node* node){
  node_destroy(node);
  free(node);
}

unsigned node_size_in_bytes(struct Node* node){
  return node->cached->inode.size;
}

struct Node* node_make_file(struct Node* dir, char* name){
  assert(dir != NULL, "node_make_file: parent node is NULL.\n");
  // ensure dir is actually a dir
  assert(node_is_dir(dir), "node_make_file: parent node is not a directory.\n");
  assert(name != NULL, "node_make_file: name is NULL.\n");
  assert(strlen(name) > 0, "node_make_file: name is empty.\n");
  assert(!ext2_name_has_separator(name),
    "node_make_file: names must be one directory entry component without '/'.\n");
  assert(!ext2_name_is_dot(name),
    "node_make_file: '.' is reserved and cannot be created as a new directory entry.\n");
  assert(!ext2_name_is_dot_dot(name),
    "node_make_file: '..' is reserved and cannot be created as a new directory entry.\n");

  // New regular files default to owner-writable, world-readable mode so the
  // extracted host artifact is readable without an extra chmod step.
  struct Node* node = alloc_inode(dir->filesystem, dir, name, EXT2_DEFAULT_FILE_MODE);
  if (node == NULL){
    return NULL;
  }

  return node;
}

struct Node* node_make_dir(struct Node* dir, char* name){
  assert(dir != NULL, "node_make_dir: parent node is NULL.\n");
  // ensure dir is actually a dir
  assert(node_is_dir(dir), "node_make_dir: parent node is not a directory.\n");
  assert(name != NULL, "node_make_dir: name is NULL.\n");
  assert(strlen(name) > 0, "node_make_dir: name is empty.\n");
  assert(!ext2_name_has_separator(name),
    "node_make_dir: names must be one directory entry component without '/'.\n");
  assert(!ext2_name_is_dot(name),
    "node_make_dir: '.' is reserved and cannot be created as a new directory entry.\n");
  assert(!ext2_name_is_dot_dot(name),
    "node_make_dir: '..' is reserved and cannot be created as a new directory entry.\n");

  // New directories default to executable/traversable permissions for all
  // readers while remaining writable only by the owner.
  struct Node* node = alloc_inode(dir->filesystem, dir, name, EXT2_DEFAULT_DIR_MODE);

  if (node == NULL){
    return NULL;
  }

  // add . and .. entries to new directory
  dir_add_entry(node, ".", node->cached->inumber, false);
  dir_add_entry(node, "..", dir->cached->inumber, false);

  return node;
}

struct Node* node_make_symlink(struct Node* dir, char* name, char* target){
  assert(dir != NULL, "node_make_symlink: parent node is NULL.\n");
  // ensure dir is actually a dir
  assert(node_is_dir(dir), "node_make_symlink: parent node is not a directory.\n");
  assert(name != NULL, "node_make_symlink: name is NULL.\n");
  assert(strlen(name) > 0, "node_make_symlink: name is empty.\n");
  assert(!ext2_name_has_separator(name),
    "node_make_symlink: names must be one directory entry component without '/'.\n");
  assert(!ext2_name_is_dot(name),
    "node_make_symlink: '.' is reserved and cannot be created as a new directory entry.\n");
  assert(!ext2_name_is_dot_dot(name),
    "node_make_symlink: '..' is reserved and cannot be created as a new directory entry.\n");
  assert(target != NULL, "node_make_symlink: target is NULL.\n");

  // Symlinks traditionally carry 0777 permissions even though most hosts ignore
  // them when dereferencing the link target.
  unsigned target_size = strlen(target);
  struct Node* node = alloc_inode(dir->filesystem, dir, name, EXT2_DEFAULT_SYMLINK_MODE);

  if (node == NULL){
    return NULL;
  }

  // ext2 fast symlinks store short targets inline in i_block instead of
  // allocating separate data blocks.
  if (target_size <= sizeof(node->cached->inode.block)) {
    memcpy((char*)node->cached->inode.block, target, target_size);
    node->cached->inode.size = target_size;
    node_sync_inode(node);
    return node;
  }

  // node_write_all expects every logical block to already exist, so allocate
  // the backing blocks for long symlink targets before copying the bytes.
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned block_count = (target_size + block_size - 1) / block_size;
  for (unsigned i = 0; i < block_count; ++i){
    bool added = node_add_block(node, alloc_block(node->filesystem));
    assert(added, "node_make_symlink: failed to allocate a data block for the symlink target.\n");
  }

  node->cached->inode.size = target_size;
  node_write_all(node, 0, target_size, target);

  return node;
}

void node_rename(struct Node* dir, char* old_name, char* new_name){
  assert(dir != NULL, "node_rename: parent node is NULL.\n");
  assert(node_is_dir(dir), "node_rename: parent node is not a directory.\n");
  assert(old_name != NULL, "node_rename: old name is NULL.\n");
  assert(new_name != NULL, "node_rename: new name is NULL.\n");
  assert(strlen(old_name) > 0, "node_rename: old name is empty.\n");
  assert(strlen(new_name) > 0, "node_rename: new name is empty.\n");
  assert(!ext2_name_has_separator(old_name),
    "node_rename: old name must be one directory entry component without '/'.\n");
  assert(!ext2_name_has_separator(new_name),
    "node_rename: new name must be one directory entry component without '/'.\n");
  assert(!ext2_name_is_dot(old_name),
    "node_rename: cannot rename the '.' directory entry.\n");
  assert(!ext2_name_is_dot_dot(old_name),
    "node_rename: cannot rename the '..' directory entry.\n");
  assert(!ext2_name_is_dot(new_name),
    "node_rename: cannot rename to the '.' directory entry.\n");
  assert(!ext2_name_is_dot_dot(new_name),
    "node_rename: cannot rename to the '..' directory entry.\n");

  if (streq(old_name, new_name)){
    return;
  }

  blocking_lock_acquire(&dir->cached->lock);
  
  struct Node* node = node_find(dir, old_name);
  
  if (dir_has_entry_name(dir, new_name)){
    panic("node_rename: a directory entry with the new name already exists in the parent directory.\n");
  }

  assert(node != NULL, 
    "node_rename: no directory entry with the old name exists in the parent directory.\n");
  
  bool rc = dir_remove_entry(dir, old_name, true);
  assert(rc, "node_rename: failed to remove the old directory entry.\n");
  rc = dir_add_entry(dir, new_name, node->cached->inumber, true);
  assert(rc, "node_rename: failed to add the new directory entry.\n");
  blocking_lock_release(&dir->cached->lock);

  node_free(node);
}

void node_delete(struct Node* dir, char* name){
  assert(dir != NULL, "node_delete: parent node is NULL.\n");
  assert(node_is_dir(dir), "node_delete: parent node is not a directory.\n");
  assert(name != NULL, "node_delete: name is NULL.\n");
  assert(strlen(name) > 0, "node_delete: name is empty.\n");
  assert(!ext2_name_has_separator(name),
    "node_delete: name must be one directory entry component without '/'.\n");
  assert(!ext2_name_is_dot(name),
    "node_delete: cannot delete the '.' directory entry.\n");
  assert(!ext2_name_is_dot_dot(name),
    "node_delete: cannot delete the '..' directory entry.\n");

  blocking_lock_acquire(&dir->cached->lock);
  
  struct Node* node = node_find(dir, name);
  
  assert(node != NULL, "node_delete: no directory entry with the given name exists in the parent directory.\n");

  if (node_is_dir(node)){
    assert(dir_is_empty(node), "node_delete: cannot delete a non-empty directory.\n");
  }

  bool rc = dir_remove_entry(dir, name, true);
  assert(rc, "node_delete: failed to remove the directory entry.\n");

  if (node_is_dir(node)){
    // directories get an extra link from their "." entry
    node->cached->inode.links_count -= 1;

    // parent directory also has a link from the child's ".." entry, so decrement that too
    dir->cached->inode.links_count -= 1;

    node_sync_inode(dir);
  }

  if (node->cached->inode.links_count > 1){
    // node has multiple links, just decrement the link count and sync the inode
    node->cached->inode.links_count -= 1;
    node_sync_inode(node);
    blocking_lock_release(&dir->cached->lock);
    node_free(node);
    return;
  }

  dealloc_inode(node);

  blocking_lock_release(&dir->cached->lock);

  node_free(node);
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
      printf("***%s\n", &name_buf);
      free(name_buf);
    }
  }
}

void read_direct_block(struct Node* node, unsigned index, char* buffer){
  assert(index < 12, "read_direct_block: index out of bounds for direct block.\n");

  read_sectors(node->filesystem, node->cached->inode.block[index], buffer);
}

void read_indirect_block(struct Node* node, unsigned index, char* buffer){
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned entries_per_block = block_size / 4;
  assert(index >= 12, "read_indirect_block: index out of bounds for indirect block.\n");
  assert(index < 12 + entries_per_block, "read_indirect_block: index out of bounds for indirect block.\n");

  unsigned real_index = index - 12;

  unsigned* direct_pointers = malloc(block_size);
  read_sectors(node->filesystem, node->cached->inode.block[12], (char*)direct_pointers);
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
  read_sectors(node->filesystem, node->cached->inode.block[13], (char*)indirect_pointers);
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
    
  read_sectors(node->filesystem, node->cached->inode.block[14], (char*)double_indirect_pointers);
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
  } else {
    panic("node_read_block: logical block index exceeds this inode addressing implementation.\n");
  }
}
  
void write_sectors(struct Ext2* fs, unsigned index, char* buffer, unsigned offset, unsigned size){
  bcache_set(&fs->bcache, index, buffer, offset, size);
}

void write_direct_block(struct Node* node, unsigned index, char* buffer, unsigned offset, unsigned size){
  assert(index < 12, "write_direct_block: index out of bounds for direct block.\n");

  write_sectors(node->filesystem, node->cached->inode.block[index], buffer, offset, size);
}

void write_indirect_block(struct Node* node, unsigned index, char* buffer, unsigned offset, unsigned size){
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned entries_per_block = block_size / 4;
  assert(index >= 12, "write_indirect_block: index out of bounds for indirect block.\n");
  assert(index < 12 + entries_per_block, "write_indirect_block: index out of bounds for indirect block.\n");

  unsigned real_index = index - 12;

  unsigned* direct_pointers = malloc(block_size);
  read_sectors(node->filesystem, node->cached->inode.block[12], (char*)direct_pointers);
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
  read_sectors(node->filesystem, node->cached->inode.block[13], (char*)indirect_pointers);
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
    
  read_sectors(node->filesystem, node->cached->inode.block[14], (char*)double_indirect_pointers);
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
  } else {
    panic("node_write_block: logical block index exceeds this inode addressing implementation.\n");
  }
}

unsigned node_read_all(struct Node* node, unsigned offset, unsigned size, char* dest){
  unsigned available;

  if (size == 0) return 0;
  if (offset >= node->cached->inode.size) return 0;

  available = node->cached->inode.size - offset;
  if (size > available){
    size = available;
  }

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

  return bytes_copied;
}

unsigned node_write_all(struct Node* node, unsigned offset, unsigned size, char* src){
  if (size == 0) return 0;

  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned start_block = offset / block_size;
  unsigned end_block = (offset + size - 1) / block_size;
  unsigned bytes_copied = 0;

  // Serialize the full write path for one inode so block growth, inode writeback,
  // and data writes observe one consistent per-file state without re-entering
  // inode_lock through icache_set().
  blocking_lock_acquire(&node->cached->lock);

  assert(node_is_file(node) || node_is_symlink(node), "node_write_all: can only write to regular files or symlinks.\n");

  // Update the file size if we wrote past the previous end of the file
  if (offset + size > node->cached->inode.size){
    // allocate new block if necessary
    while (end_block >= node->cached->data_block_count){
      bool added = node_add_block(node, alloc_block(node->filesystem));
      assert(added, "node_write_all: failed to allocate a new block for the file data.\n");
    }

    node->cached->inode.size = offset + size;
  }

  node_sync_inode(node);

  for (unsigned i = start_block; i <= end_block; ++i){
    unsigned block_offset = (i == start_block) ? offset % block_size : 0;
    unsigned copy_size = (i == end_block) ? ((offset + size - 1) % block_size) - block_offset + 1 : block_size - block_offset;
    node_write_block(node, i, src + bytes_copied, block_offset, copy_size);

    bytes_copied += copy_size;
  }
  blocking_lock_release(&node->cached->lock);

  return size;
}

unsigned short node_get_type(struct Node* node){
  return node->cached->inode.mode & 0xF000;
}

bool node_is_dir(struct Node* node){
  unsigned short type = node->cached->inode.mode & 0xF000;
  return (type == EXT2_S_IFDIR);
}

bool node_is_file(struct Node* node){
  unsigned short type = node->cached->inode.mode & 0xF000;
  return (type == EXT2_S_IFREG);
}

bool node_is_symlink(struct Node* node){
  unsigned short type = node->cached->inode.mode & 0xF000;
  return (type == EXT2_S_IFLNK);
}

void node_get_symlink_target(struct Node* node, char* dest){
  assert(node_is_symlink(node), "node_get_symlink_target: node is not a symlink.\n");

  if (node->cached->inode.size <= sizeof(node->cached->inode.block)) {
    assert(node->cached->inode.size <= sizeof(node->cached->inode.block),
      "node_get_symlink_target: fast symlink size exceeds inline inode storage.\n");
    memcpy(dest, &node->cached->inode.block, node->cached->inode.size);
    *(dest + node->cached->inode.size) = 0;
  } else {
    node_read_all(node, 0, node->cached->inode.size, dest);
    *(dest + node->cached->inode.size) = 0;
  }
}

unsigned node_get_num_links(struct Node* node){
  return node->cached->inode.links_count;
}

unsigned node_entry_count(struct Node* node){
  assert(node_is_dir(node), "node_entry_count: node is not a directory.\n");

  blocking_lock_acquire(&node->cached->lock);

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

  blocking_lock_release(&node->cached->lock);

  return count;
}
