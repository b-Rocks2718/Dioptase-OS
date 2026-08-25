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
The block cache is a 32-line write-through cache keyed by ext2 logical block
number and uses a simple LRU age scheme. Reads populate the cache on miss, and
writes update the cached block image and disk together.

#### VM Page Cache
File-backed virtual mappings use the separate VM page cache in `page_cache.c`.
It keys resident physical pages by inode and page index so mappings of the same
file page can share the loaded data. A writable shared fault, rather than a
plain cache acquisition, max-merges the byte extent eligible for final dirty
writeback, so read-only aliases cannot extend the file accidentally. This cache
is distinct from the ext2 logical-block cache above.

#### Path Lookup
`node_find()` resolves a pathname starting from a directory or symlink node. Absolute paths restart from the ext2 root. An empty path returns the starting inode as a fresh heap-owned wrapper. Multi-component traversal is supported, symlinks are expanded during traversal, relative symlink targets are resolved relative to the symlink's containing directory, and lookup aborts after 100 symlink expansions to avoid infinite loops.

The user-visible `open_existing()` syscall is a lookup-only wrapper around this
operation. It copies and validates the complete user path, then either installs
a descriptor for the resolved inode or returns `-1`; a missing component never
calls a create helper. The older `open()` syscall deliberately retains its
create-on-miss behavior for output paths.

#### Regular File Reads
Reads are EOF-clamped and may start and end at arbitrary byte offsets. Logical
block lookup supports direct, single-indirect, double-indirect, and
triple-indirect addressing. Sparse holes at any level read as zero-filled
blocks. `node_read_block()` reads one complete logical block without applying
EOF, while `node_read_all()` is the normal EOF-clamped byte-range API.

#### Regular File Writes
`node_write_all()` writes arbitrary byte ranges and grows the file as needed. A
nonzero request whose `offset + size` exclusive end cannot be represented in
32 bits returns zero before block arithmetic or allocation. File growth
allocates data blocks before issuing writes, updates inode size, and preserves
zero-filled gaps because newly allocated blocks are cleared before use. Inode
or block exhaustion during growth returns zero without updating the logical
file size or writing the requested bytes; create helpers similarly return
`NULL` and roll back any unpublished child inode. The write path is serialized
per inode, so concurrent writers to the same file do not race the block tree or
inode writeback.

`node_shrink()` reduces a regular file's logical size and writes the smaller
inode size back to disk. It rejects growth requests, but it intentionally does
not deallocate data blocks or clear truncated bytes. If the file later regrows
into the preserved allocation before those bytes are overwritten, the old tail
contents become visible again. The truncate syscall uses
`vmem_truncate_file()` rather than calling `node_shrink()` directly: the VM
wrapper holds the page-cache lock across the inode shrink and caps or clears
every matching live dirty entry before allowing a final cache release.

#### Directories
Directories use normal ext2 rev 0 directory records. New entries are added either by reusing slack space in an existing record or by allocating a new directory block. Deleted entries leave holes with `inode == 0`, and later creates may reuse those holes. `node_make_dir()` initializes `.` and `..`, and `node_delete()` only allows directory deletion when the directory is empty except for those two entries.

The named `EXT2_MAX_NAME_BYTES` limit is 255 bytes per directory-entry component. The directory writer asserts this as an internal invariant, while public regular-file, directory, and symlink create helpers return `NULL` for an overlong basename before allocating an inode or block. A 255-byte basename is valid; 256 bytes is not.

Directory iteration accepts only offset zero, exact EOF, or the beginning of an ext2 directory record. An interior or beyond-EOF offset is rejected. If the next live entry does not fit in the caller's buffer, the returned continuation offset remains at that entry so a later call retries it; deleted records may be consumed without producing a user-visible entry.

Tested in `ext_new_file.c` and `ext_delete.c`

#### Symlinks
Symlink targets are stored verbatim and may be absolute or relative. Fast symlinks are stored inline in `inode.block[]` when the target is 60 bytes or less. Longer targets are stored in normal data blocks and read back through the same block-tree logic as regular files.

Callers that need both a target size and its bytes use `node_copy_symlink_target()`. It allocates and copies a NUL-terminated snapshot while holding the inode lock, preventing a concurrent internal writer from changing the required capacity between the size read and the copy. Malformed inode metadata reporting a target size of `UINT_MAX` returns `NULL` before the terminator addition can wrap; traversal treats that symlink as unresolvable.

#### Create
The filesystem currently supports creating regular files, directories, and symlinks. Create names must be one non-empty directory entry component of at most 255 bytes without `/`, and `.` / `..` are reserved. Duplicate-name detection and insertion are serialized under the parent directory lock, so concurrent creates of the same basename produce exactly one winner. Overlong names are rejected before allocation, including when a later component of a creating `open()` path is overlong; earlier missing parents are not partially created. `open_existing()` validates the same component limit but never creates parents or a final file.

The same parent lock covers the complete create transaction. A new inode may be inserted into the internal inode cache while it is being built, but it remains unreachable through directory traversal until directory `.` / `..` records or all symlink target bytes have been initialized and the final child inode has been written. Installing the parent directory entry is the final namespace-publication step. Consequently a concurrent lookup/delete/rename/create on that parent observes either no entry or a fully initialized child.

#### Rename
`node_rename()` currently only supports same-directory rename. Renaming onto an existing target name is rejected, and renaming a name to itself is a no-op. Successful rename preserves inode identity and file contents because it mutates only the parent directory entries.

#### Delete / Lifetime
`node_delete()` returns zero after removing an entry and `-1` when the name is invalid/missing, the parent has already been unlinked, or the target is a non-empty directory. It releases every temporary lookup wrapper on both success and failure. The directory entry disappears immediately on success, but final inode and block reclamation is deferred until the last live wrapper for that inode is released. This is tracked with the inode cache `refcount` plus the `delete_pending` flag.

Syscall paths use `node_delete_typed()`: `rmdir()` requests an empty directory, while `unlink()` requests a regular file or symlink. The parent-directory lock spans exact-name lookup, target-kind validation, the empty-directory check, and removal. A competing delete/create/rename therefore cannot replace the validated inode with another kind before commit. The untyped `node_delete()` wrapper remains available for internal callers.

#### Locking
The filesystem uses several lock layers:
- `metadata_lock` protects superblock, block-group descriptor, and bitmap writes
- `inode_lock` protects inode-table read-modify-write cycles
- each cached inode has its own blocking lock protecting size, block-tree, link-count, and `delete_pending`
- inode cache and block cache each have their own internal lock
- operations spanning the VM page cache and an inode always take the global
  page-cache lock before the cached-inode lock; ext code has no inverse path

Create and delete follow a parent-directory-then-child lock order. A new child is not namespace-reachable while both locks may be held. The parent lock remains held across blocking storage operations and final publication; typed deletion retains it across target validation and removal. These are blocking locks, so namespace transactions are serialized without globally disabling interrupts.

### Not Yet Supported
- Hard links
- Cross-directory rename
- Block reclamation during file shrinking
- rwx permission enforcement
- uid / gid
- atime / mtime / ctime updates
- VFS layer
- coherence between live VM page-cache entries and direct file reads/writes
  that bypass that cache

### Tests
- `ext_read.c`
- `ext_new_file.c`
- `ext_write.c`
- `ext_delete.c`
- `ext_rename.c`
