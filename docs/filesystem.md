## File System

The kernel filesystem is an in-kernel ext2 rev 0 implementation backed by SD drive 1. There is no VFS layer right now. The implementation uses a refcounted inode cache, a small write-through logical block cache, and blocking locks around namespace and inode mutations.

### Supported Filesystem Features

#### Mount / Geometry
`ext2_init()` reads the superblock, block group descriptor table, inode bitmaps, and block bitmaps into memory, then opens the root inode through the inode cache as `fs.root`. The current test harness supports ext2 block sizes of 1024, 2048, and 4096 bytes.

#### Node Wrappers
The public API revolves around `struct Node`, which is one wrapper around a shared cached inode plus traversal context such as `parent_inumber`. `fs.root` is embedded in the `struct Ext2`, while successful `node_find()` calls return heap-owned wrappers that must be released with `node_free()`.

#### Inode Cache
The inode cache is shared across the whole filesystem instance. Cache entries are reference-counted so multiple `Node` wrappers can share the same inode. Cache misses publish a placeholder entry first, then concurrent missers wait on a gate until the inode contents have been read from disk and marked valid.

#### Block Cache
The block cache is a small write-through cache keyed by ext2 logical block number. It currently has 12 cache lines and uses a simple LRU age scheme. Reads populate the cache on miss, and writes update the cached block image and disk together.

#### Path Lookup
`node_find()` resolves a pathname starting from a directory or symlink node. Absolute paths restart from the ext2 root. An empty path returns the starting inode as a fresh heap-owned wrapper. Multi-component traversal is supported, symlinks are expanded during traversal, relative symlink targets are resolved relative to the symlink's containing directory, and lookup aborts after 100 symlink expansions to avoid infinite loops.

#### Regular File Reads
Reads are EOF-clamped and may start and end at arbitrary byte offsets. Logical block lookup supports direct, single-indirect, double-indirect, and triple-indirect addressing. `node_read_block()` assumes the requested logical block already exists, while `node_read_all()` is the safe high-level API for normal reads.

#### Regular File Writes
`node_write_all()` writes arbitrary byte ranges and grows the file as needed. File growth allocates data blocks before issuing writes, updates inode size, and preserves zero-filled gaps because newly allocated blocks are cleared before use. The write path is serialized per inode, so concurrent writers to the same file do not race the block tree or inode writeback.

#### Directories
Directories use normal ext2 rev 0 directory records. New entries are added either by reusing slack space in an existing record or by allocating a new directory block. Deleted entries leave holes with `inode == 0`, and later creates may reuse those holes. `node_make_dir()` initializes `.` and `..`, and `node_delete()` only allows directory deletion when the directory is empty except for those two entries.

Tested in `ext_new_file.c` and `ext_delete.c`

#### Symlinks
Symlink targets are stored verbatim and may be absolute or relative. Fast symlinks are stored inline in `inode.block[]` when the target is 60 bytes or less. Longer targets are stored in normal data blocks and read back through the same block-tree logic as regular files.

#### Create
The filesystem currently supports creating regular files, directories, and symlinks. Create names must be one non-empty directory entry component without `/`, and `.` / `..` are reserved. Duplicate-name detection and insertion are serialized under the parent directory lock, so concurrent creates of the same basename produce exactly one winner.

#### Rename
`node_rename()` currently only supports same-directory rename. Renaming onto an existing target name is rejected, and renaming a name to itself is a no-op. Successful rename preserves inode identity and file contents because it mutates only the parent directory entries.

#### Delete / Lifetime
`node_delete()` removes the directory entry immediately, but final inode and block reclamation is deferred until the last live wrapper for that inode is released. This is tracked with the inode cache `refcount` plus the `delete_pending` flag.

#### Locking
The filesystem uses several lock layers:
- `metadata_lock` protects superblock, block-group descriptor, and bitmap writes
- `inode_lock` protects inode-table read-modify-write cycles
- each cached inode has its own blocking lock protecting size, block-tree, link-count, and `delete_pending`
- inode cache and block cache each have their own internal lock

### Not Yet Supported
- Hard links
- Cross-directory rename
- Truncate / file shrinking
- rwx permission enforcement
- uid / gid
- atime / mtime / ctime updates
- VFS layer or page cache

### Tests
- `ext_read.c`
- `ext_new_file.c`
- `ext_write.c`
- `ext_delete.c`
- `ext_rename.c`
