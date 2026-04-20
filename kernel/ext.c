#include "ext.h"
#include "sd_driver.h"
#include "print.h"
#include "debug.h"
#include "heap.h"
#include "string.h"

struct Ext2 fs;

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
static struct Node* dir_find_entry(struct Node* dir, char* name);
static struct Node* dir_find_entry_locked(struct Node* dir, char* name);

// Helpers to do filesystem operations when the caller already holds node->cached->lock
static bool dir_is_empty_locked(struct Node* dir);
static void node_read_block_locked(struct Node* node, unsigned block_num, char* dest);
static unsigned node_read_all_locked(struct Node* node, unsigned offset, unsigned size, char* dest);
static void node_write_block_locked(struct Node* node, unsigned block_num, char* src, unsigned offset, unsigned size);

static void dealloc_inode(struct Node* node);

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

  // ext2 files in this implementation only grow by appending blocks, so each
  // addressing tier is packed from the front. That lets us count blocks by
  // stopping at the first zero pointer in each tier.
  while (count < 12 && node->cached->inode.block[count] != 0){
    count += 1;
  }

  // A short direct run means the file never reached any indirect tier.
  if (count < 12 || node->cached->inode.block[12] == 0){
    return count;
  }

  single_indirect = malloc(block_size);
  // Count the single-indirect leaf. If it is not full, the append-only layout
  // guarantees there cannot be any live blocks in deeper tiers yet.
  bcache_get(&node->filesystem->bcache, node->cached->inode.block[12], (char*)single_indirect);
  single_count = ext2_count_indirect_entries(single_indirect, entries_per_block);
  count += single_count;
  if (single_count < entries_per_block || node->cached->inode.block[13] == 0){
    free(single_indirect);
    return count;
  }

  double_indirect = malloc(block_size);
  bcache_get(&node->filesystem->bcache, node->cached->inode.block[13], (char*)double_indirect);
  // Each live entry in the double-indirect root points at one single-indirect
  // leaf. Stop when we reach an unused root slot or a partially-filled leaf.
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
  // The triple-indirect walk follows the same packed layout one tier deeper:
  // stop at the first unused outer pointer or the first partially-filled leaf.
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
  // Bootstrap the in-memory ext2 view from disk before any cache or node code
  // runs. After this, higher-level helpers can assume the descriptor table,
  // allocation bitmaps, and root inode are available.
  // start by reading superblock
  int rc = sd_read_blocks(SD_DRIVE_1, 2, 2, &fs->superblock);
  assert(rc == 0, "ext2_init: failed to read ext2 superblock.\n");

  // check this is an ext2 file system
  if (fs->superblock.magic != 0xEF53){
    say("| Filesystem superblock magic does not identify an ext2 image.\n", NULL);
    fs->initialized = false;
    return;
  }
  
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
  assert((root_inode->inode.mode & EXT2_S_MASK) == EXT2_S_IFDIR,
    "ext2_init: root inode is not a directory.\n");

  node_init(&fs->root, root_inode, EXT2_BAD_INO, fs);

  fs->initialized = true;
}

void ext2_destroy(struct Ext2* fs){
  // called on an Ext2 that didnt successfully initialize
  if (!fs->initialized) return;

  fs->initialized = false;

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
  bcache_destroy(&fs->bcache);
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

// Consume one queued path component and linearly scan ext2's rec_len-linked
// directory records for a live entry with that exact name.
struct Node* ext2_enter_dir(struct Ext2* fs, struct Node* dir, struct RingBuf* path){
  char* name = ringbuf_remove_back(path);
  struct Node* node;

  assert(name != NULL, "ext2_enter_dir: path is empty.\n");

  node = dir_find_entry(dir, name);
  free(name);
  return node;
}

// expand one symlink into path components and choose the next traversal base
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

  // Walk the unresolved path one component at a time. Directories consume one
  // queued component, while symlinks replace the current component with their
  // target path and restart from the correct base directory.
  while (ringbuf_size(path) > 0) {
    if (dir == NULL) {
      ext2_free_pending_path(path);
      ext2_release_owned_node(parent, parent_owned);
      return NULL;
    }
    // Bound symlink expansion so self-referential links fail cleanly instead of
    // looping forever.
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

      // A normal directory step advances one level: current dir becomes parent,
      // and the looked-up child becomes the new current node.
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

      // Symlink traversal does not consume the next queued component. Instead it
      // rewrites the remaining path and restarts from root or the containing
      // directory, depending on whether the target was absolute or relative.
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
      
      // The group summary counters are just a hint. Walk the actual bitmap to
      // claim one concrete free inode inside the selected group.
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

      if ((mode & EXT2_S_MASK) == EXT2_S_IFDIR){
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

  if ((mode & EXT2_S_MASK) == EXT2_S_IFDIR){
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
      
      // After choosing a group by its summary counter, scan that group's bitmap
      // to locate the exact logical block number to allocate.
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
  bool fast_symlink = (node->cached->inode.mode & EXT2_S_MASK) == EXT2_S_IFLNK &&
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
  cached->inode.links_count = (mode & EXT2_S_MASK) == EXT2_S_IFDIR ? 2 : 1; // directory has extra link for ".", file has 1 link
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
  // ext2 i_blocks counts every reserved filesystem block in 512-byte sectors:
  // the new data block plus any newly-allocated pointer blocks.
  unsigned reserved_blocks = 1;

  // This helper only appends at EOF. `logical_block` is therefore the next
  // empty data slot, and each branch allocates whatever metadata blocks are
  // needed to make that slot reachable before storing `block_num`.
  if (logical_block < 12){
    // Direct region: inode.block[0..11] points straight at data blocks.
    node->cached->inode.block[logical_block] = block_num;
    node->cached->inode.blocks += reserved_blocks * sectors_per_block;
    node->cached->data_block_count += 1;
    return true;
  } else if (logical_block < single_limit){
    unsigned* single_indirect = malloc(block_size);
    // The first append past the direct region also needs the single-indirect
    // pointer block itself.
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

    // Store the new data block in the next free slot of the single-indirect leaf.
    single_indirect[logical_block - 12] = block_num;
    bcache_set(&node->filesystem->bcache, node->cached->inode.block[12], (char*)single_indirect, 0, block_size);
    node->cached->inode.blocks += reserved_blocks * sectors_per_block;
    node->cached->data_block_count += 1;
    free(single_indirect);
    return true;
  } else if (logical_block < double_limit){
    // Rebase the logical index so 0 means "first block in the double-indirect
    // region", then split that index into the root slot and leaf slot.
    unsigned double_index = logical_block - single_limit;
    unsigned indirect_index = double_index / entries_per_block;
    unsigned direct_index = double_index % entries_per_block;
    unsigned* double_indirect = malloc(block_size);
    unsigned* single_indirect = malloc(block_size);

    // Double-indirect growth may need two metadata allocations: the top-level
    // double-indirect block, and a leaf single-indirect block for this span.
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

    // A leaf slot of 0 means this append is the first entry in a new
    // single-indirect leaf under the double-indirect root.
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

    // Once the metadata path exists, the final write is just one leaf update.
    single_indirect[direct_index] = block_num;
    bcache_set(&node->filesystem->bcache, double_indirect[indirect_index], (char*)single_indirect, 0, block_size);
    node->cached->inode.blocks += reserved_blocks * sectors_per_block;
    node->cached->data_block_count += 1;
    free(double_indirect);
    free(single_indirect);
    return true;
  } else if (logical_block < triple_limit){
    // Rebase into the triple-indirect region, then split the index into
    // top-level, middle-level, and leaf offsets.
    unsigned triple_index = logical_block - double_limit;
    unsigned outer_index = triple_index / double_span;
    unsigned middle_index = (triple_index / entries_per_block) % entries_per_block;
    unsigned direct_index = triple_index % entries_per_block;
    unsigned* triple_indirect = malloc(block_size);
    unsigned* double_indirect = malloc(block_size);
    unsigned* single_indirect = malloc(block_size);

    // Triple-indirect growth follows the same pattern one level deeper.
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

    // Every multiple of one full double-span starts a new double-indirect node
    // hanging from the triple-indirect root.
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

    // Every 0 leaf offset starts a new single-indirect node under that
    // double-indirect subtree.
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

    // After the metadata chain is present, publish the new data block in the leaf.
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
// Caller must hold dir->cached->lock.
static bool dir_insert_entry_in_existing_block(struct Node* dir, unsigned block_index, char* name, unsigned inumber){
  unsigned block_size = ext2_get_block_size(dir->filesystem);
  unsigned new_entry_size = ext2_dir_entry_min_size(strlen(name));
  char* block_buf = malloc(block_size);

  assert(dir->cached->lock.is_held,
    "dir_insert_entry_in_existing_block: caller must hold the directory lock.\n");
  node_read_block_locked(dir, block_index, block_buf);

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
      // Deleted entries leave holes with inode == 0. Reuse the hole directly
      // if its record is already large enough for the new name.
      if (record_length >= new_entry_size){
        ext2_write_dir_entry(block_buf + offset, record_length, name, inumber);
        node_write_block_locked(dir, block_index, block_buf, 0, block_size);
        free(block_buf);
        return true;
      }
    } else {
      unsigned ideal_length = ext2_dir_entry_min_size(existing->name_len);
      // Live entries may own slack at the tail of their record. Shrink this
      // record to its ideal size and place the new entry in the freed suffix.
      if (record_length >= ideal_length + new_entry_size){
        existing->rec_len = ideal_length;
        ext2_write_dir_entry(block_buf + offset + ideal_length,
          record_length - ideal_length, name, inumber);
        node_write_block_locked(dir, block_index, block_buf, 0, block_size);
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

// Add an entry to a directory while the caller already owns the directory lock.
// This keeps same-directory create/rename/delete sequences atomic with respect
// to competing directory traversals.
static bool dir_add_entry_locked(struct Node* dir, char* name, unsigned inumber){
  unsigned block_size = ext2_get_block_size(dir->filesystem);
  unsigned logical_block_count = dir->cached->data_block_count;

  assert(node_is_dir(dir), "dir_add_entry: target node is not a directory.\n");
  assert(strlen(name) > 0, "dir_add_entry: directory entries must have a non-empty name.\n");
  assert(!dir->cached->delete_pending,
    "dir_add_entry: cannot add entries to an unlinked directory.\n");
  assert(dir->cached->lock.is_held,
    "dir_add_entry_locked: caller must hold the directory lock.\n");

  for (unsigned i = 0; i < logical_block_count; ++i){
    if (dir_insert_entry_in_existing_block(dir, i, name, inumber)){
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
  node_write_block_locked(dir, logical_block_count, block_buf, 0, block_size);
  free(block_buf);

  dir->cached->inode.size += block_size;
  node_sync_inode(dir);

  return true;
}

// Add an entry to a directory, acquiring the directory lock internally for
// callers that are not already inside a larger directory mutation transaction.
static bool dir_add_entry(struct Node* dir, char* name, unsigned inumber){
  blocking_lock_acquire(&dir->cached->lock);
  bool rc = dir_add_entry_locked(dir, name, inumber);
  blocking_lock_release(&dir->cached->lock);
  return rc;
}

// Remove an entry from a directory while the caller already owns the directory
// lock. The lock stays held across the full scan and potential record merge.
static bool dir_remove_entry_locked(struct Node* dir, char* name){
  unsigned block_size = ext2_get_block_size(dir->filesystem);
  unsigned logical_block_count = dir->cached->data_block_count;

  assert(node_is_dir(dir), "dir_remove_entry: target node is not a directory.\n");
  assert(name != NULL, "dir_remove_entry: name is NULL.\n");
  assert(!dir->cached->delete_pending,
    "dir_remove_entry: cannot remove entries from an unlinked directory.\n");
  assert(dir->cached->lock.is_held,
    "dir_remove_entry_locked: caller must hold the directory lock.\n");

  for (unsigned i = 0; i < logical_block_count; ++i){
    char* block_buf = malloc(block_size);

    node_read_block_locked(dir, i, block_buf);

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
        // ext2 deletions clear the inode field. If there is a previous record,
        // merge this record's rec_len into it so later inserts see one
        // contiguous reusable hole.
        existing->inode = 0;
        if (prev != NULL){
          prev->rec_len += existing->rec_len;
        }
        node_write_block_locked(dir, i, block_buf, 0, block_size);
        free(block_buf);

        return true;
      }
      

      offset += record_length;
      prev = existing;
    }

    assert(offset == block_size,
      "dir_remove_entry: directory block did not terminate at the block boundary.\n");

    free(block_buf);
  }

  return false;
}

// Remove an entry from a directory, acquiring the directory lock internally for
// callers that are not already in a larger mutation sequence.
static bool dir_remove_entry(struct Node* dir, char* name){
  blocking_lock_acquire(&dir->cached->lock);
  bool rc = dir_remove_entry_locked(dir, name);
  blocking_lock_release(&dir->cached->lock);
  return rc;
}

// Look up one exact basename while the caller already owns the directory lock.
// Successful lookups take an extra cached-inode reference before returning so
// the result remains valid after the caller drops the directory lock.
static struct Node* dir_find_entry_locked(struct Node* dir, char* name){
  unsigned index = 0;
  unsigned name_len = strlen(name);
  struct DirEntry entry;

  assert(node_is_dir(dir), "dir_find_entry: target node is not a directory.\n");
  assert(name != NULL, "dir_find_entry: name is NULL.\n");
  assert(dir->cached->lock.is_held,
    "dir_find_entry_locked: caller must hold the directory lock.\n");

  while (index < node_size_in_bytes(dir)){
    int cnt = node_read_all_locked(dir, index, sizeof(struct DirEntry), (char*)&entry);
    assert(cnt >= 4, "dir_find_entry: failed to read directory entry.\n");
    assert(entry.rec_len >= EXT2_DIR_ENTRY_HEADER_SIZE,
      "dir_find_entry: invalid directory record length.\n");

    index += entry.rec_len;
    if (entry.inode != 0 && entry.name_len == name_len &&
        strneq((char*)entry.name, name, name_len)) {
      struct CachedInode* inode = icache_get(&dir->filesystem->icache, entry.inode);
      struct Node* node = malloc(sizeof(struct Node));

      node_init(node, inode, dir->cached->inumber, dir->filesystem);
      return node;
    }
  }

  return NULL;
}

// Look up one exact basename, acquiring the directory lock internally for
// callers that are only traversing the namespace.
static struct Node* dir_find_entry(struct Node* dir, char* name){
  blocking_lock_acquire(&dir->cached->lock);
  struct Node* node = dir_find_entry_locked(dir, name);
  blocking_lock_release(&dir->cached->lock);
  return node;
}

// Duplicate names are rejected before allocation so failed creates do not
// consume inode numbers or mutate the parent directory on disk.
static bool dir_has_entry_name(struct Node* dir, char* name){
  unsigned index = 0;
  unsigned name_len = strlen(name);
  struct DirEntry entry;

  assert(node_is_dir(dir), "dir_has_entry_name: target node is not a directory.\n");
  assert(name != NULL, "dir_has_entry_name: name is NULL.\n");
  assert(dir->cached->lock.is_held,
    "dir_has_entry_name: caller must hold the parent directory lock.\n");

  while (index < node_size_in_bytes(dir)){
    int cnt = node_read_all_locked(dir, index, sizeof(struct DirEntry), (char*)&entry);
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
// Caller must hold dir->cached->lock
static bool dir_is_empty_locked(struct Node* dir){
  assert(dir->cached->lock.is_held,
    "dir_is_empty: caller must hold the candidate directory lock.\n");

  unsigned index = 0;
  struct DirEntry entry;

  assert(node_is_dir(dir), "dir_is_empty: target node is not a directory.\n");
  assert(dir->cached->lock.is_held,
    "dir_is_empty: caller must hold the candidate directory lock.\n");

  while (index < node_size_in_bytes(dir)){
    int cnt = node_read_all_locked(dir, index, sizeof(struct DirEntry), (char*)&entry);
    assert(cnt >= 4, "dir_is_empty: failed to read directory entry.\n");
    assert(entry.rec_len >= EXT2_DIR_ENTRY_HEADER_SIZE,
      "dir_is_empty: invalid directory record length.\n");
    assert(entry.rec_len % EXT2_DIR_ENTRY_ALIGN_SIZE == 0,
      "dir_is_empty: directory record is not 4-byte aligned.\n");

    if (entry.inode != 0){
      bool is_dot = entry.name_len == 1 && strneq((char*)entry.name, ".", 1);
      bool is_dot_dot = entry.name_len == 2 && strneq((char*)entry.name, "..", 2);

      if (!is_dot && !is_dot_dot){
        return false;
      }
    }

    index += entry.rec_len;
  }

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

  if (dir->cached->delete_pending){
    blocking_lock_release(&dir->cached->lock);
    return NULL;
  }

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
  // Publish the new inode in the shared cache before linking it into the
  // directory tree so later lookups can reuse the same cached object.
  icache_insert(&fs->icache, cached);

  struct Node* node = malloc(sizeof(struct Node));

  node_init(node, cached, dir->cached->inumber, fs);

  // inode allocated successfully, now update parent directory
  bool added = dir_add_entry_locked(dir, name, inumber);
  assert(added, "alloc_inode: failed to add the new directory entry to the parent directory.\n");

  if ((mode & EXT2_S_MASK) == EXT2_S_IFDIR){
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
  assert(node->cached->delete_pending,
    "dealloc_inode: inode must be pending delete before reclamation.\n");
  assert(node->cached->inode.links_count == 0,
    "dealloc_inode: inode still has live directory links.\n");

  node_dealloc_blocks(node);
  dealloc_inumber(node->filesystem, node->cached->inumber, node->cached->inode.mode);
}

static void cached_inode_init(struct CachedInode* cached, unsigned inumber){
  cached->inumber = inumber;
  cached->refcount = 1;
  cached->valid = false; // invalid until the inode data is read in from disk
  cached->delete_pending = false;
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

// Publish a placeholder cache entry before doing disk IO so concurrent misses
// wait on the same gate instead of reading the same inode twice.
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

    // The entry may still be mid-fill if another thread published the
    // placeholder but has not copied the inode data out of disk yet.
    gate_wait(&cached->valid_gate);

    return cached;
  } 
  
  blocking_lock_release(&cache->lock);

  // insert entry into cache before reading, so we avoid reading twice
  struct CachedInode* new_cache_entry = malloc(sizeof(struct CachedInode));
  cached_inode_init(new_cache_entry, inumber);

  blocking_lock_acquire(&cache->lock);
  // Another thread may have published the same placeholder while this thread
  // was allocating its own candidate entry.
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

  // Copy the inode into the published placeholder, then mark it valid and wake
  // every waiter that raced on the same miss.
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

    // The inode lock serializes the final reclaim decision against the unlink
    // path that sets delete_pending and writes the terminal link count.
    blocking_lock_acquire(&cached->lock);

    if (cached->delete_pending){
      struct Node node;
      node.cached = cached;
      node.parent_inumber = EXT2_BAD_INO;
      node.filesystem = cache->fs;
      dealloc_inode(&node);
    }

    blocking_lock_release(&cached->lock);
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
  cache->block_cache = malloc(BCACHE_SIZE * block_size);
  for (unsigned i = 0; i < BCACHE_SIZE; ++i){
    // Clear tags so a freshly initialized cache cannot report a stale hit.
    cache->tags[i] = -1;
    cache->ages[i] = i;
  }
}

// On a miss, drop the cache lock for SD IO and then install the fetched block
// back into the cache before returning the data to the caller.
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
    // Lower ages mean "more recently used". On a hit, age every hotter entry
    // by one step and reset this entry back to most-recently-used.
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

  // Reinstall the fetched block into the oldest cache line now that the IO is
  // complete and the cache lock can be held again.
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

// Write-through path: update the cached block image if present, then push the
// final bytes to disk so later readers observe the new contents.
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
    // Keep the cached copy and the disk write buffer identical, then write the
    // entire logical block back through to disk.
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

  // Install the freshly-read block into the chosen cache line before patching
  // the requested byte range, so the cache retains a full coherent block image.
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

void bcache_destroy(struct BlockCache* cache){
  free(cache->block_cache);
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

struct Node* node_clone(struct Node* node){
  if (node == NULL){
    return NULL;
  }

  struct Node* clone = malloc(sizeof(struct Node));

  *clone = *node;

  // dont need to acquire lock, we know that refcount will stay > 1 
  // for the duration of this call, as the caller holds a reference to the node
  __atomic_fetch_add((int*)&node->cached->refcount, 1);

  return clone;
}

void node_destroy(struct Node* node){
  icache_release(&node->filesystem->icache, node->cached);
}

void node_free(struct Node* node){
  if (node == NULL || node == &fs.root) return;
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
  dir_add_entry(node, ".", node->cached->inumber);
  dir_add_entry(node, "..", dir->cached->inumber);

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

  // Same-directory rename is implemented as remove+add under the parent lock,
  // so no other thread can observe the old name removed and then reuse the new
  // name before the replacement entry is installed.
  blocking_lock_acquire(&dir->cached->lock);
  assert(!dir->cached->delete_pending,
    "node_rename: cannot mutate a directory that has already been unlinked.\n");
  
  struct Node* node = dir_find_entry_locked(dir, old_name);
  
  if (dir_has_entry_name(dir, new_name)){
    panic("node_rename: a directory entry with the new name already exists in the parent directory.\n");
  }

  assert(node != NULL, 
    "node_rename: no directory entry with the old name exists in the parent directory.\n");
  
  bool rc = dir_remove_entry_locked(dir, old_name);
  assert(rc, "node_rename: failed to remove the old directory entry.\n");
  rc = dir_add_entry_locked(dir, new_name, node->cached->inumber);
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

  // Hold the parent directory lock across lookup, unlink, and link-count
  // updates so the directory entry stream stays stable during deletion.
  blocking_lock_acquire(&dir->cached->lock);
  assert(!dir->cached->delete_pending,
    "node_delete: cannot mutate a directory that has already been unlinked.\n");
  
  struct Node* node = dir_find_entry_locked(dir, name);
  
  assert(node != NULL, "node_delete: no directory entry with the given name exists in the parent directory.\n");

  // Serialize the candidate inode against concurrent reads, writes, and, for
  // directories, against creates through already-open wrappers.
  blocking_lock_acquire(&node->cached->lock);

  if (node_is_dir(node)){
    assert(dir_is_empty_locked(node), "node_delete: cannot delete a non-empty directory.\n");
  }

  bool rc = dir_remove_entry_locked(dir, name);
  assert(rc, "node_delete: failed to remove the directory entry.\n");

  if (node_is_dir(node)){
    // parent directory also has a link from the child's ".." entry, so decrement that too
    dir->cached->inode.links_count -= 1;
    node_sync_inode(dir);
    assert(node->cached->inode.links_count == 2,
      "node_delete: empty directory should only have '.' and its parent link before final unlink.\n");
    // The directory loses both the parent entry and its self-link once delete
    // commits. Reclaim the inode later, after every open wrapper releases it.
    node->cached->inode.links_count = 0;
  } else {
    assert(node->cached->inode.links_count > 0,
      "node_delete: file link count underflowed during delete.\n");
    node->cached->inode.links_count -= 1;
  }

  if (node->cached->inode.links_count == 0){
    node->cached->delete_pending = true;
  }

  node_sync_inode(node);

  blocking_lock_release(&node->cached->lock);

  blocking_lock_release(&dir->cached->lock);

  node_free(node);
}

void read_sectors(struct Ext2* fs, unsigned index, char* buffer){
  bcache_get(&fs->bcache, index, buffer);
}

// ext2 regular files may leave zero block pointers to represent sparse holes.
// Reading through those holes must return zero-filled bytes rather than
// falling through to filesystem block 0.
static void read_sectors_or_zero(struct Node* node, unsigned block_num, char* buffer){
  unsigned block_size = ext2_get_block_size(node->filesystem);

  if (block_num == 0){
    memset(buffer, 0, block_size);
    return;
  }

  read_sectors(node->filesystem, block_num, buffer);
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

  read_sectors_or_zero(node, node->cached->inode.block[index], buffer);
}

void read_indirect_block(struct Node* node, unsigned index, char* buffer){
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned entries_per_block = block_size / 4;
  assert(index >= 12, "read_indirect_block: index out of bounds for indirect block.\n");
  assert(index < 12 + entries_per_block, "read_indirect_block: index out of bounds for indirect block.\n");

  // Strip off the 12 direct blocks so the remaining index addresses one entry
  // inside the single-indirect pointer block.
  unsigned real_index = index - 12;

  unsigned* direct_pointers = malloc(block_size);
  if (node->cached->inode.block[12] == 0){
    memset(buffer, 0, block_size);
    free(direct_pointers);
    return;
  }

  read_sectors(node->filesystem, node->cached->inode.block[12], (char*)direct_pointers);
  read_sectors_or_zero(node, direct_pointers[real_index], buffer);
  free(direct_pointers);
}

void read_double_indirect_block(struct Node* node, unsigned index, char* buffer){
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned entries_per_block = block_size / 4;
  assert(index >= 12 + entries_per_block, "read_double_indirect_block: index out of bounds for double indirect block.\n");
  assert(index < 12 + entries_per_block * (1 + entries_per_block), "read_double_indirect_block: index out of bounds for double indirect block.\n");

  // Skip past the direct and single-indirect ranges, then split the remaining
  // index into the outer indirect slot and the final direct slot.
  unsigned real_index = index - (12 + entries_per_block);

  unsigned* indirect_pointers = malloc(block_size);
  unsigned* direct_pointers = malloc(block_size);
  if (node->cached->inode.block[13] == 0){
    memset(buffer, 0, block_size);
    free(indirect_pointers);
    free(direct_pointers);
    return;
  }

  read_sectors(node->filesystem, node->cached->inode.block[13], (char*)indirect_pointers);
  if (indirect_pointers[real_index / entries_per_block] == 0){
    memset(buffer, 0, block_size);
    free(indirect_pointers);
    free(direct_pointers);
    return;
  }

  read_sectors(node->filesystem, indirect_pointers[real_index / entries_per_block], (char*)direct_pointers);
  read_sectors_or_zero(node, direct_pointers[real_index % entries_per_block], buffer);
  free(indirect_pointers);
  free(direct_pointers);
}

void read_triple_indirect_block(struct Node* node, unsigned index, char* buffer){
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned entries_per_block = block_size / 4;
  assert(index >= 12 + entries_per_block * (1 + entries_per_block), "read_triple_indirect_block: index out of bounds for triple indirect block.\n");
  assert(index < 12 + entries_per_block * (1 + entries_per_block * (1 + entries_per_block)), "read_triple_indirect_block: index out of bounds for triple indirect block.\n");

  // Skip the shallower tiers, then decompose the remaining index into
  // triple-indirect, double-indirect, and final direct offsets.
  unsigned real_index = index - (12 + entries_per_block * (1 + entries_per_block));

  unsigned* double_indirect_pointers = malloc(block_size);
  unsigned* indirect_pointers = malloc(block_size);
  unsigned* direct_pointers = malloc(block_size);
  if (node->cached->inode.block[14] == 0){
    memset(buffer, 0, block_size);
    free(double_indirect_pointers);
    free(indirect_pointers);
    free(direct_pointers);
    return;
  }

  read_sectors(node->filesystem, node->cached->inode.block[14], (char*)double_indirect_pointers);
  if (double_indirect_pointers[real_index / (entries_per_block * entries_per_block)] == 0){
    memset(buffer, 0, block_size);
    free(double_indirect_pointers);
    free(indirect_pointers);
    free(direct_pointers);
    return;
  }

  read_sectors(node->filesystem, double_indirect_pointers[real_index / (entries_per_block * entries_per_block)], (char*)indirect_pointers);
  if (indirect_pointers[(real_index / entries_per_block) % entries_per_block] == 0){
    memset(buffer, 0, block_size);
    free(double_indirect_pointers);
    free(indirect_pointers);
    free(direct_pointers);
    return;
  }

  read_sectors(node->filesystem, indirect_pointers[(real_index / entries_per_block) % entries_per_block], (char*)direct_pointers);
  read_sectors_or_zero(node, direct_pointers[real_index % entries_per_block], buffer);
  
  free(double_indirect_pointers);
  free(indirect_pointers);
  free(direct_pointers);
}

// Caller must hold node->cached->lock
static void node_read_block_locked(struct Node* node, unsigned block_num, char* dest){
  assert(node->cached->lock.is_held, 
    "node_read_block_locked: caller must hold the inode lock across the read operation.\n");

  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned entries_per_block = block_size / 4;

  // Dispatch through the same 12 direct / single / double / triple-indirect
  // tiers that ext2 stores in inode.block[].
  if (block_num < 12) read_direct_block(node, block_num, dest);
  else if (block_num < 12 + entries_per_block) read_indirect_block(node, block_num, dest);
  else if (block_num < 12 + entries_per_block * (1 + entries_per_block)) read_double_indirect_block(node, block_num, dest);
  else if (block_num < 12 + entries_per_block * (1 + entries_per_block * (1 + entries_per_block))){
    read_triple_indirect_block(node, block_num, dest);
  } else {
    panic("node_read_block: logical block index exceeds this inode addressing implementation.\n");
  }
}

void node_read_block(struct Node* node, unsigned block_num, char* dest){
  blocking_lock_acquire(&node->cached->lock);
  node_read_block_locked(node, block_num, dest);
  blocking_lock_release(&node->cached->lock);
}
  
void write_sectors(struct Ext2* fs, unsigned index, char* buffer, unsigned offset, unsigned size){
  bcache_set(&fs->bcache, index, buffer, offset, size);
}

void write_direct_block(struct Node* node, unsigned index, char* buffer, unsigned offset, unsigned size){
  assert(index < 12, "write_direct_block: index out of bounds for direct block.\n");
  assert(node->cached->inode.block[index] != 0,
    "write_direct_block: caller must materialize sparse holes before writing.\n");

  write_sectors(node->filesystem, node->cached->inode.block[index], buffer, offset, size);
}

void write_indirect_block(struct Node* node, unsigned index, char* buffer, unsigned offset, unsigned size){
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned entries_per_block = block_size / 4;
  assert(index >= 12, "write_indirect_block: index out of bounds for indirect block.\n");
  assert(index < 12 + entries_per_block, "write_indirect_block: index out of bounds for indirect block.\n");

  // Strip off the direct region so the remaining index addresses one entry
  // inside the single-indirect pointer block.
  unsigned real_index = index - 12;

  unsigned* direct_pointers = malloc(block_size);
  assert(node->cached->inode.block[12] != 0,
    "write_indirect_block: caller must materialize the indirect block before writing.\n");
  read_sectors(node->filesystem, node->cached->inode.block[12], (char*)direct_pointers);
  assert(direct_pointers[real_index] != 0,
    "write_indirect_block: caller must materialize sparse holes before writing.\n");
  write_sectors(node->filesystem, direct_pointers[real_index], buffer, offset, size);
  free(direct_pointers);
}

void write_double_indirect_block(struct Node* node, unsigned index, char* buffer, unsigned offset, unsigned size){
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned entries_per_block = block_size / 4;
  assert(index >= 12 + entries_per_block, "read_double_indirect_block: index out of bounds for double indirect block.\n");
  assert(index < 12 + entries_per_block * (1 + entries_per_block), "read_double_indirect_block: index out of bounds for double indirect block.\n");

  // Skip past the direct and single-indirect ranges, then split the remaining
  // index into the outer indirect slot and the final direct slot.
  unsigned real_index = index - (12 + entries_per_block);

  unsigned* indirect_pointers = malloc(block_size);
  unsigned* direct_pointers = malloc(block_size);
  assert(node->cached->inode.block[13] != 0,
    "write_double_indirect_block: caller must materialize the double-indirect root before writing.\n");
  read_sectors(node->filesystem, node->cached->inode.block[13], (char*)indirect_pointers);
  assert(indirect_pointers[real_index / entries_per_block] != 0,
    "write_double_indirect_block: caller must materialize the indirect leaf before writing.\n");
  read_sectors(node->filesystem, indirect_pointers[real_index / entries_per_block], (char*)direct_pointers);
  assert(direct_pointers[real_index % entries_per_block] != 0,
    "write_double_indirect_block: caller must materialize sparse holes before writing.\n");
  write_sectors(node->filesystem, direct_pointers[real_index % entries_per_block], buffer, offset, size);
  free(indirect_pointers);
  free(direct_pointers);
}

void write_triple_indirect_block(struct Node* node, unsigned index, char* buffer, unsigned offset, unsigned size){
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned entries_per_block = block_size / 4;
  assert(index >= 12 + entries_per_block * (1 + entries_per_block), "write_triple_indirect_block: index out of bounds for triple indirect block.\n");
  assert(index < 12 + entries_per_block * (1 + entries_per_block * (1 + entries_per_block)), "write_triple_indirect_block: index out of bounds for triple indirect block.\n");

  // Skip the shallower tiers, then decompose the remaining index into
  // triple-indirect, double-indirect, and final direct offsets.
  unsigned real_index = index - (12 + entries_per_block * (1 + entries_per_block));

  unsigned* double_indirect_pointers = malloc(block_size);
  unsigned* indirect_pointers = malloc(block_size);
  unsigned* direct_pointers = malloc(block_size);
  assert(node->cached->inode.block[14] != 0,
    "write_triple_indirect_block: caller must materialize the triple-indirect root before writing.\n");
  read_sectors(node->filesystem, node->cached->inode.block[14], (char*)double_indirect_pointers);
  assert(double_indirect_pointers[real_index / (entries_per_block * entries_per_block)] != 0,
    "write_triple_indirect_block: caller must materialize the double-indirect leaf before writing.\n");
  read_sectors(node->filesystem, double_indirect_pointers[real_index / (entries_per_block * entries_per_block)], (char*)indirect_pointers);
  assert(indirect_pointers[(real_index / entries_per_block) % entries_per_block] != 0,
    "write_triple_indirect_block: caller must materialize the indirect leaf before writing.\n");
  read_sectors(node->filesystem, indirect_pointers[(real_index / entries_per_block) % entries_per_block], (char*)direct_pointers);
  assert(direct_pointers[real_index % entries_per_block] != 0,
    "write_triple_indirect_block: caller must materialize sparse holes before writing.\n");
  write_sectors(node->filesystem, direct_pointers[real_index % entries_per_block], buffer, offset, size);
  
  free(double_indirect_pointers);
  free(indirect_pointers);
  free(direct_pointers);
}

// Caller must hold node->cached->lock
static void node_write_block_locked(struct Node* node, unsigned block_num, char* src,
    unsigned offset, unsigned size){
  assert(node->cached->lock.is_held, 
    "node_write_block_locked: caller must hold the inode lock across the write operation.\n");
  unsigned block_size = ext2_get_block_size(node->filesystem);
  unsigned entries_per_block = block_size / 4;

  // Writes use the same addressing tiers as reads; `node_write_all(...)`
  // guarantees the logical block already exists before dispatch reaches here.
  if (block_num < 12) write_direct_block(node, block_num, src, offset, size);
  else if (block_num < 12 + entries_per_block) write_indirect_block(node, block_num, src, offset, size);
  else if (block_num < 12 + entries_per_block * (1 + entries_per_block)) write_double_indirect_block(node, block_num, src, offset, size);
  else if (block_num < 12 + entries_per_block * (1 + entries_per_block * (1 + entries_per_block))){
    write_triple_indirect_block(node, block_num, src, offset, size);
  } else {
    panic("node_write_block: logical block index exceeds this inode addressing implementation.\n");
  }
}

void node_write_block(struct Node* node, unsigned block_num, char* src, unsigned offset, unsigned size){
  blocking_lock_acquire(&node->cached->lock);
  node_write_block_locked(node, block_num, src, offset, size);
  blocking_lock_release(&node->cached->lock);
}

// Caller must hold node->cached->lock
static unsigned node_read_all_locked(struct Node* node, unsigned offset, unsigned size, char* dest){
  assert(node->cached->lock.is_held, 
    "node_read_all_locked: caller must hold the inode lock across the read operation.\n");
  
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

  // Reads may start and end mid-block, so copy block-by-block and clamp the
  // first and last block to just the requested byte range.
  for (unsigned i = start_block; i <= end_block; ++i){
    char* block_buf = malloc(block_size);
    node_read_block_locked(node, i, block_buf);

    unsigned block_offset = (i == start_block) ? offset % block_size : 0;
    unsigned copy_size = (i == end_block) ? ((offset + size - 1) % block_size) - block_offset + 1 : block_size - block_offset;

    memcpy(dest + bytes_copied, block_buf + block_offset, copy_size);
    bytes_copied += copy_size;
    free(block_buf);
  }

  return bytes_copied;
}

unsigned node_read_all(struct Node* node, unsigned offset, unsigned size, char* dest){
  unsigned cnt;

  blocking_lock_acquire(&node->cached->lock);
  cnt = node_read_all_locked(node, offset, size, dest);
  blocking_lock_release(&node->cached->lock);

  return cnt;
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

  // Host-built ext2 images may encode a trailing run of all-zero file blocks as
  // sparse holes with zero block pointers. Materialize that missing tail before
  // any write so the lower-level block writers never fall through to block 0.
  while (end_block >= node->cached->data_block_count){
    bool added = node_add_block(node, alloc_block(node->filesystem));
    assert(added, "node_write_all: failed to allocate a new block for the file data.\n");
  }

  // Update the file size if we are going to write past the previous end of the file.
  if (offset + size > node->cached->inode.size){
    node->cached->inode.size = offset + size;
  }

  node_sync_inode(node);

  for (unsigned i = start_block; i <= end_block; ++i){
    unsigned block_offset = (i == start_block) ? offset % block_size : 0;
    unsigned copy_size = (i == end_block) ? ((offset + size - 1) % block_size) - block_offset + 1 : block_size - block_offset;
    node_write_block_locked(node, i, src + bytes_copied, block_offset, copy_size);

    bytes_copied += copy_size;
  }
  blocking_lock_release(&node->cached->lock);

  return size;
}

unsigned short node_get_type(struct Node* node){
  return node->cached->inode.mode & EXT2_S_MASK;
}

bool node_is_dir(struct Node* node){
  unsigned short type = node->cached->inode.mode & EXT2_S_MASK;
  return (type == EXT2_S_IFDIR);
}

bool node_is_file(struct Node* node){
  unsigned short type = node->cached->inode.mode & EXT2_S_MASK;
  return (type == EXT2_S_IFREG);
}

bool node_is_symlink(struct Node* node){
  unsigned short type = node->cached->inode.mode & EXT2_S_MASK;
  return (type == EXT2_S_IFLNK);
}

void node_get_symlink_target(struct Node* node, char* dest){
  assert(node_is_symlink(node), "node_get_symlink_target: node is not a symlink.\n");

  blocking_lock_acquire(&node->cached->lock);

  if (node->cached->inode.size <= sizeof(node->cached->inode.block)) {
    // Fast symlink: the target bytes live inline in inode.block[].
    assert(node->cached->inode.size <= sizeof(node->cached->inode.block),
      "node_get_symlink_target: fast symlink size exceeds inline inode storage.\n");
    memcpy(dest, &node->cached->inode.block, node->cached->inode.size);
    *(dest + node->cached->inode.size) = 0;
  } else {
    // Long symlink: read the target out of the inode's data blocks like file data.
    node_read_all_locked(node, 0, node->cached->inode.size, dest);
    *(dest + node->cached->inode.size) = 0;
  }

  blocking_lock_release(&node->cached->lock);
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
    node_read_all_locked(node, index, sizeof(struct DirEntry), (char*)&entry);
    assert(entry.rec_len >= 8, "node_entry_count: invalid directory record length.\n");
    index += entry.rec_len;
    if (entry.inode != 0) count++;
  }

  blocking_lock_release(&node->cached->lock);

  return count;
}

// Writes a linux_dirent structure from a DirEntry into the given buffer.
// Returns number of bytes written.
int write_dirent(struct Ext2* fs, struct DirEntry entry, char* buffer_start, unsigned remaining_size) {
  unsigned reclen = sizeof(struct linux_dirent) + entry.name_len + 1; // +1 for d_type.

  // Align to 4 bytes.
  reclen = (reclen + 3) & ~3;
  if (reclen > remaining_size) {
    return 0; // Not enough space.
  }

  struct linux_dirent* dirent = (struct linux_dirent*) buffer_start;
  dirent->d_ino = entry.inode;
  dirent->d_off = 0; // Unused.
  dirent->d_reclen = reclen;
  memcpy(&dirent->d_name, entry.name, entry.name_len);
  *(&dirent->d_name + entry.name_len) = 0; // Null-terminate name.

  // d_type.
  struct CachedInode* cached_inode = icache_get(&fs->icache, entry.inode);
  char type = EXT2_DT_UNKNOWN;
  if (cached_inode != NULL) {
    unsigned short mode = cached_inode->inode.mode;
    switch (mode & EXT2_S_MASK) {
      case EXT2_S_IFDIR:
        type = EXT2_DT_DIR;
        break;
      case EXT2_S_IFREG:
        type = EXT2_DT_REG;
        break;
      case EXT2_S_IFLNK:
        type = EXT2_DT_LNK;
        break;
    }
  }
  *((char*)dirent + reclen - 1) = type;
  return reclen;
}

int node_getdents(struct Node* dir, unsigned offset, char* buffer, unsigned buffer_size, int* new_offset) {
  assert(node_is_dir(dir), "node_getdents: node is not a directory.\n");

  blocking_lock_acquire(&dir->cached->lock);

  unsigned index = 0;
  struct DirEntry entry;

  unsigned current_offset = 0;
  unsigned total_bytes_read = 0;
  char* buffer_pointer = buffer;
  
  while (index < node_size_in_bytes(dir)) {
    int cnt = node_read_all_locked(dir, index, sizeof(struct DirEntry), (char*) &entry);
    assert(cnt >= 4, "node_getdents: failed to read directory entry.\n");
    assert(entry.rec_len >= 8, "node_getdents: invalid directory record length.\n");
  
    index += entry.rec_len;
    if (entry.inode == 0 || current_offset + entry.rec_len <= offset) {
      // Empty entry or not at desired offset yet.
      current_offset += entry.rec_len;
      continue;
    }

    // Write dirent into buffer.
    int bytes_written = write_dirent(dir->filesystem, entry, buffer_pointer, buffer_size - total_bytes_read);
    total_bytes_read += bytes_written;
    buffer_pointer += bytes_written;
    current_offset += entry.rec_len;

    if (bytes_written == 0) {
      // Buffer full.
      break;
    }
  }
  blocking_lock_release(&dir->cached->lock);
  *new_offset = current_offset;
  return total_bytes_read;
}
