## Syscalls

User programs enter the kernel with `trap` using the trap ABI documented in
`../../docs/abi.md`. The trap instruction itself is documented in
`../../docs/ISA.md`.

- `r1`: trap code
- `r2-r8`: trap-specific arguments
- return value: `r1` unless otherwise documented
- IVT entry `0x004`: shared trap vector

The current Dioptase-OS trap code assignments are defined in
`../kernel/sys.h` and implemented in `../kernel/sys.c`.

### Descriptor Namespaces

- file descriptors occupy slots `0..99`
- semaphore descriptors occupy slots `100..199`
- child descriptors occupy slots `200..299`

User-entering threads start with:

- `STDIN = 0`
- `STDOUT = 1`
- `STDERR = 2`

Those slots are ordinary file-descriptor table entries. If a process closes one
of them, later `open()`, `pipe()`, or `dup()` calls may reuse that numeric slot.

### User Pointers

Syscalls that accept user pointers validate the complete user-side range before
copying. A valid user range must:

- live in the user virtual address range
- be covered by user VMEs in the current thread
- have read permission for kernel copies from user memory
- have write permission for kernel copies to user memory

Invalid user pointers return `-1`.

Path arguments are copied as bounded NUL-terminated strings. Paths use the
filesystem traversal rules documented in `filesystem.md`: absolute paths start
at the ext2 root, while relative paths start at the calling thread's current
working directory.

Current implementation-defined bounds:

- path arguments: at most 1024 bytes including the terminating NUL
- `read()` / `write()` byte count per trap: at most 1024 bytes
- `pipe()` ring-buffer capacity: 1024 bytes
- `execv()` argument count: at most 64
- each `execv()` argument string: at most 256 bytes including the terminating
  NUL
- rebuilt `execv()` argv block must fit in the initial 16 KiB user stack

### Process, Time, and Scheduling

| Code | Wrapper | Arguments | Result |
| --- | --- | --- | --- |
| `0` | `exit(status)` | `status` | Terminates the current user image. On success this trap does not return to user mode; `wait_child()` later returns the child's exit status. |
| `1` | `test_syscall(arg)` | `arg` | Test-only trap. Emits `***test_syscall arg = <arg>` and returns `arg + 7`. |
| `2` | `get_current_jiffies()` | none | Returns the current global jiffy counter. |
| `13` | `sleep(jiffies)` | `jiffies` | Blocks the caller for at least the requested number of jiffies, including across 32-bit jiffy wrap, then returns `0`. Raw trap requests above `INT_MAX` return `-1`; the current CRT wrapper has a `void` result. |
| `23` | `fork()` | none | Returns `0` in the child. Returns a child descriptor in `200..299` in the parent. Returns `-1` on failure. The child inherits the parent's cwd, entry-time descriptor tables, and user address space. Private resident pages are snapshotted; direct physical/MMIO mappings remain live aliases. The new descriptor returned by this fork is parent-only and is not present in the child. |
| `24` | `execv(path, argc, argv)` | `path`, `argc`, `argv` | Replaces the current user image. Success does not return. Failure returns `-1`. If `argc == 0`, `argv` is ignored and the new image starts with `argc = 0`, `argv = NULL`. |
| `27` | `wait_child(child_desc)` | `child_desc` | Blocks until the specified child exits, returns that child's exit status, then consumes the child descriptor. Re-waiting the same descriptor returns `-1`. |
| `32` | `yield()` | none | Voluntarily yields the CPU and returns `0`. |
| `47` | `signal_child(child_desc, signal)` | `child_desc`, `signal` | Records `signal` for the specified live child. Returns `0`, or `-1` for an invalid child descriptor, exited child, or signal outside `0..31`. |
| `49` | `request_priority(priority)` | `priority` | Requests a static scheduler priority for the current thread. Returns `0` and updates the thread when `priority` is valid, or `-1` for an invalid priority. |
| `50` | `set_foreground_child(child_desc)` | `child_desc` | Sets the single terminal foreground child to the caller's live child descriptor and returns `0`, or clears it when `child_desc == -1`. Clearing returns `1` if that foreground child used a direct display trap and otherwise returns `0`. An invalid or already-exited descriptor passed while setting returns `-1`. |
| `51` | `signal_foreground(signal)` | `signal` | Records `signal` for the current foreground child. Returns `0`, or `-1` if no live foreground child is set or `signal` is outside `0..31`. |
| `52` | `register_handler(signal, handler)` | `signal`, `handler` | Registers a four-byte-aligned executable user entry point for signals `0..30`. Returns `0`, or `-1` for an invalid signal, `SIGNAL_KILL`, a misaligned address, or an address outside executable user memory. |
| `53` | `sigreturn(rc)` | `rc` | From a signal handler, completes that handler and resumes the suspended user context. A valid call does not return to the wrapper. Outside a handler it returns `-1`. |
| `54` | `mask_signal(signal)` | `signal` | Masks a signal in `0..15`, retaining any pending instance. The operation is idempotent and returns `0`; invalid or nonmaskable signals return `-1`. |
| `55` | `unmask_signal(signal)` | `signal` | Unmasks a signal in `0..15`. The operation is idempotent and returns `0`; invalid or nonmaskable signals return `-1`. |

`request_priority()` currently honors all valid requests without any permission
checks. Valid user priority values are `DIOPTASE_PRIORITY_LOW = 0`,
`DIOPTASE_PRIORITY_NORMAL = 1`, and `DIOPTASE_PRIORITY_HIGH = 2`. The request
does not reset the caller's current MLFQ level or remaining quantum; those
dynamic scheduler fields continue to follow the scheduler rules documented in
`scheduling.md`.

`signal_child()` replaces the former `kill()` wrapper, and
`signal_foreground(signal)` replaces `kill_foreground()`. The foreground form
now takes an explicit signal number, just like `signal_child()`.

The shell sets one foreground child while waiting for an external
command; the terminal uses
`signal_foreground(SIGNAL_TERMINATE)` to turn Ctrl-C into termination
of that child instead of delivering byte `0x03` to the input pipe. Ordinary
keyboard input still reaches the foreground command through the inherited
`STDIN` pipe.

Signal numbers, delivery order, masking, handler arguments, and synchronous
fault behavior are specified in `signals.md`.

Each foreground child descriptor records whether its live child used a direct
display trap. The display traps are codes `4..10`, `36`, `37`, and `43..46`;
invalid sprite-number requests do not set the claim. Calls made by the terminal,
shell, or a background child do not set the foreground descriptor's claim.
`set_text_color()` is console output state and does not count as a direct
display claim. The claim remains available after child exit until the shell
clears the foreground slot and receives it as the clear operation's return
value.

### Exec

- `execv()` snapshots the user `argv[]` array and every argument string before
  destroying the old address space.
- The new image starts with `argc` in `r1` and the rebuilt `argv` pointer in
  `r2`, matching the normal function-call ABI for `main(int argc, char** argv)`.
- The rebuilt `argv[]` contains a trailing `NULL` entry.

For `argc > 0`, `argv` must point to a readable user array of `argc` readable
NUL-terminated strings. Invalid pointers, unterminated strings, or oversized
argument vectors return `-1`.

`execv()` currently accepts only regular files whose contents pass the kernel's
ELF32 sanity checks. Directories, symlinks, empty files, and malformed or
non-ELF regular files return `-1`.

The accepted executable layout is an implementation-defined subset matching the
in-tree Dioptase assembler:

- the ELF identity is 32-bit, little-endian, current-version System V ABI 0;
  `e_type` is executable, `e_machine` is `EM_DIOPTASE`, and the ELF header and
  program-header entry sizes exactly match the kernel's ELF32 structures
- the complete program-header table must follow the ELF header and fit in the
  file without offset or size arithmetic wrapping; the implementation-defined
  program-header count is `1..64` (the in-tree assembler emits three), bounding
  both pairwise layout validation and the number of VMEs one exec may request
- non-`PT_LOAD` program headers are ignored; their mapping-related fields are
  not interpreted
- a zero-length `PT_LOAD` is inert and is accepted so the assembler can emit an
  empty rodata segment at the same file/virtual address as the following data
  segment; its zero-byte file range and remaining header fields are still
  validated
- every `PT_LOAD` has only `PF_R`/`PF_W`/`PF_X` permission bits,
  `p_align == 4096`, and a 4096-byte-aligned user address
- each nonempty `PT_LOAD` additionally has `p_filesz <= p_memsz`, an in-file
  payload, and a page-rounded range representable by the kernel's 32-bit VME
  endpoints; `p_offset` itself need not be page aligned because the loader
  copies bytes into an anonymous mapping
- page-rounded nonempty load ranges cannot overlap one another or the top-down
  16 KiB ordinary-stack plus 4 KiB signal-stack reservation
- `e_entry` is four-byte aligned and its complete first instruction lies in the
  logical byte range of a nonempty executable load segment

Exec stages an immutable heap snapshot of the validated file bytes before
touching the caller's address space. It then constructs the replacement
address space beside the old one and commits only after every nonempty
`PT_LOAD`, both stacks, and the rebuilt argv are installed. Construction
failure destroys the in-progress address space, restores the pre-exec image,
and returns `-1` without terminating the caller. Concurrent writers therefore
cannot change the bytes that will be loaded, and a late load error cannot
leave the process without an address space.

### Filesystem, Pipes, and Audio

| Code | Wrapper | Arguments | Result |
| --- | --- | --- | --- |
| `14` | `open(path)` | `path` | Resolves `path` from the current cwd unless the path is absolute. Creates the file if it does not exist. Returns a file descriptor in `0..99`, or `-1` on copy, overlong component, creation, or descriptor-allocation failure. |
| `56` | `open_existing(path)` | `path` | Resolves an existing file, directory, or symlink target with the same cwd/absolute traversal rules as `open()`, but never creates a missing component. Returns a file descriptor in `0..99`, or `-1` on copy, overlong component, missing path, or descriptor-allocation failure. Trap ABI: code `56` in `r1`, `path` in `r2`, and the descriptor/result in `r1`. |
| `15` | `read(fd, buf, count)` | `fd`, `buf`, `count` | Copies up to the clamped byte count into `buf`. Returns the number of bytes read, `0` at file/pipe EOF, or `-1` on failure. A pipe read validates the complete clamped destination before consuming data. A process whose fd `0` still names the default stdin descriptor blocks waiting for keyboard input; if fd `0` has been replaced with a pipe or file, it follows that descriptor's normal read behavior. |
| `16` | `write(fd, buf, count)` | `fd`, `buf`, `count` | Copies up to the clamped byte count from `buf`. Returns the number of bytes written or `-1` on failure. A pipe write interrupted by final-reader close returns its positive committed prefix, or `-1` if it committed no byte. `STDOUT` and `STDERR` write characters to the console; each clamped write is one serialized console transaction and cannot interleave with an independently owned transaction on another core. A same-core interrupt diagnostic may nest without deadlocking; while a VGA transaction is interrupted, its diagnostic bytes are routed to UART instead of touching VGA state. |
| `17` | `close(fd)` | `fd` | Closes a valid file descriptor and returns `0`, or returns `-1` for an invalid descriptor. |
| `25` | `play_audio_file(fd)` | `fd` | Validates a supported WAV through a playback worker, starts asynchronous playback, and returns `0`. Returns `-1` if `fd` is invalid, names another descriptor kind, names an empty file, cannot be mapped by the worker, or contains a malformed/unsupported WAV. |
| `28` | `chdir(path)` | `path` | Resolves `path` relative to the current cwd unless absolute, requires the result to be a directory, updates the cwd, and returns `0`. Returns `-1` on copy or lookup failure, or if the target is not a directory. |
| `29` | `pipe(fds)` | `fds` | Allocates a pipe and writes `{read_fd, write_fd}` into the user array `fds[0..1]`. Returns `0` on success or `-1` on copy or descriptor-allocation failure. |
| `30` | `dup(fd)` | `fd` | Returns a new file descriptor that references the same underlying descriptor object, including the shared current offset. Returns `-1` on failure. |
| `31` | `seek(fd, offset, whence)` | `fd`, `offset`, `whence` | Updates a normal filesystem descriptor's offset and returns the new offset, or returns `-1` for an invalid descriptor, a pipe/console descriptor, or invalid `whence`. |
| `33` | `getdents(fd, buffer, buffer_size)` | `fd`, `buffer`, `buffer_size` | Copies up to the buffer size of full directory entries (`struct linux_dirent` format) into `buffer`. Increments the descriptor offset by bytes consumed from the directory file (not by the differently sized output records). Returns the number of bytes copied or `-1` if the descriptor/offset is invalid, the descriptor is not a directory, or `buffer_size` is zero. |
| `34` | `getcwd(buffer, buffer_size)` | `buffer`, `buffer_size` | Copies the current cwd path plus trailing NUL into `buffer`. Returns `buffer` on success or `(char*)-1` on invalid user memory, missing cwd state, or insufficient buffer size. |
| `35` | `readlink(path, buffer, buffer_size)` | `path`, `buffer`, `buffer_size` | Copies up to the clamped byte count of the target of a symbolic link plus trailing NUL into `buffer`. Returns the number of bytes copied or `-1` on missing or invalid symlink. |
| `38` | `fd_bytes_available(fd)` | `fd` | Returns a nonblocking snapshot of the number of queued bytes for a read or write pipe endpoint, including `0` for an empty open pipe or drained EOF. Returns `-1` for an invalid or non-pipe descriptor. |
| `39` | `truncate(fd, size)` | `fd`, `size` | Shrinks a regular file descriptor to `size` bytes and returns `0`, or returns `-1` for an invalid descriptor, a non-regular-file descriptor, or any request that would grow the file. The EOF update is serialized with live shared-mapping dirty writeback. |
| `40` | `mkdir(path)` | `path` | Creates one empty subdirectory entry in the current cwd and returns `0`, or returns `-1` on invalid user memory, invalid/overlong name, duplicate basename, or create failure. |
| `41` | `rmdir(path)` | `path` | Removes one empty subdirectory entry from the current cwd and returns `0`, or returns `-1` if the target is missing, is not a directory, is not empty, or the name is invalid. |
| `42` | `unlink(path)` | `path` | Removes one regular-file or symbolic-link entry from the current cwd and returns `0`, or returns `-1` if the target is missing, has another inode kind, or the name is invalid. |

Additional file-descriptor notes:
- Console serialization covers kernel-managed output and text state. A process
  using a direct tilemap/framebuffer mapping bypasses that lock and must define
  its own coordination with console output.
- The tile scale and horizontal/vertical scroll set/move traps use that same
  serialization. In particular, a move is one locked MMIO read/modify/write and
  cannot lose an update to console scrolling on another core.
- `dup()` shares the same underlying descriptor object, so offset changes are
  visible through both the original descriptor and the duplicate.
- Pipe reads retain the existing exact-count blocking behavior while writers
  remain open. After the final write endpoint closes, buffered data drains and
  that read may return a short positive count; later reads return EOF (`0`).
- Closing the final read endpoint wakes blocked writers. Such a write returns
  the positive byte prefix it already committed, or `-1` if it committed
  nothing. A zero-byte write retains the general syscall behavior and returns
  `0` without testing the opposite endpoint.
- A pipe side remains open while any descriptor-table reference names its one
  logical endpoint object. Because `dup()` and `fork()` share that object,
  closing one duplicate or one process's inherited slot does not publish side
  closure while another such reference remains.
- `seek()` uses `SEEK_SET = 0`, `SEEK_CUR = 1`, and `SEEK_END = 2`.
- `SEEK_SET` rejects negative offsets.
- `SEEK_CUR` rejects results that would be negative or would exceed signed
  32-bit range.
- `SEEK_END` rejects results that would be negative or would exceed signed
  32-bit range.
- Pipe and console descriptors are not seekable; every `whence` returns `-1`
  without changing their descriptor state.
- A `getdents()` offset must be zero, exact directory EOF, or the start of an
  ext2 directory record. Interior and beyond-EOF offsets return `-1`; exact EOF
  returns `0`. If the next live entry does not fit, the call does not consume
  it, including when no entry fits at all.
- `truncate()` is shrink-only. It leaves descriptor offsets unchanged and does
  not reclaim blocks. While holding the VM page-cache lock, it updates the inode
  EOF and caps dirty writeback for the partial page at that EOF, while
  discarding dirty state for cached pages wholly beyond it. Releasing a mapping
  dirtied before truncate cannot restore the old size. A later writable shared
  page fault may extend the file again under the existing mmap contract.
- `mkdir()`, `rmdir()`, and `unlink()` are currently basename-only wrappers
  over the ext2 helpers. `path` must be one non-empty component in the current
  working directory; slash-separated and absolute paths return `-1`.
- `mkdir()`, `rmdir()`, and `unlink()` reject `.` and `..`.
- Every filesystem path component is limited to 255 bytes. `open()` and
  `open_existing()` validate the complete bounded path. Creating `open()` does
  so before it creates any missing parent; lookup-only `open_existing()` never
  enters a create path. Basename-only mutation calls reject a 256-byte name.
  Exactly 255 bytes is accepted.
- `unlink()` currently applies to regular files and symlinks only; use
  `rmdir()` for empty directories.
- `rmdir()` and `unlink()` retain the parent-directory lock across lookup,
  target-kind validation, empty-directory validation where applicable, and
  removal. Concurrent namespace replacement cannot make either syscall delete
  an inode kind that its public contract rejects.
- Successful `unlink()` and `rmdir()` remove the pathname immediately, but
  final inode/block reclamation may still be deferred until the last live
  wrapper for that inode is released.
- `play_audio_file()` submits playback to one persistent audio daemon because
  kernel VM mappings belong to one TCB/address space. The syscall waits only
  until that daemon has mapped and validated the exact private mapping it will
  retain. Mapping failure, truncation, malformed RIFF/chunk bounds, an empty or
  odd-sized data payload, unsupported format fields, and four already-
  outstanding requests (queued plus playing) return `-1` with an inode/size
  diagnostic when applicable. After a `0` result, playback proceeds
  asynchronously from the retained mapping; the
  descriptor may be closed without revoking the daemon's source. Playback waits
  for device progress with an implementation-defined 30,000-jiffy deadline.
  Accepted playback holds a kernel asynchronous-work reference until all
  daemon-owned mappings, Nodes, and request state have been cleaned, so global
  shutdown cannot overtake the asynchronous operation.
- The accepted hardware format is signed 16-bit little-endian mono PCM at
  exactly 25,000 samples/second. The WAV `byte_rate` and `block_align` fields
  must agree with that fixed format.
### Synchronization and Virtual Memory

| Code | Wrapper | Arguments | Result |
| --- | --- | --- | --- |
| `18` | `sem_open(count)` | `count` | Allocates a semaphore descriptor in `100..199`, initializes it to `count`, and returns that descriptor. Returns `-1` for negative counts or exhaustion. |
| `19` | `sem_up(sem)` | `sem` | Increments the semaphore and returns `0`, or returns `-1` for an invalid semaphore descriptor or if a count already at `INT_MAX` cannot be incremented. |
| `20` | `sem_down(sem)` | `sem` | Decrements the semaphore, blocking if needed, and returns `0`, or returns `-1` for an invalid semaphore descriptor. |
| `21` | `sem_close(sem)` | `sem` | Closes a semaphore descriptor and returns `0`, or returns `-1` for an invalid semaphore descriptor. |
| `22` | `mmap(size, fd, offset, flags)` | `size`, `fd`, `offset`, `flags` | Returns the base address of the new user mapping, or `-1` on failure. `fd == MMAP_ANON` requests an anonymous mapping. |

`mmap()` details that matter to user mode:

- user-visible flags come from `root/crt/sys.h`: `MMAP_SHARED`, `MMAP_READ`,
  `MMAP_WRITE`, and `MMAP_EXEC`
- the kernel always adds the internal `MMAP_USER` bit to user-created mappings
- size must be nonzero, fit in the positive signed trap-argument range, and be
  representable after 4096-byte page rounding
- `MMAP_ANON` (`-1`) is the only anonymous descriptor sentinel; anonymous
  mappings require offset zero and anonymous shared mappings are rejected
- file-backed mappings require a normal descriptor naming a regular file, a
  non-negative 4096-byte-aligned offset, and a representable offset-plus-size
- unknown flag bits are rejected rather than silently ignored
- directory and pipe descriptors are not mappable
- inability to find a sufficiently large user virtual range returns `-1`
- a shared writable page fault conservatively authorizes writeback through the
  byte extent exposed by that mapping; writable aliases max-merge that extent,
  while read-only aliases do not enlarge it
- shared mappings are coherent with each other through the VM page cache, but
  direct `read()` / `write()` paths currently bypass that cache and do not have
  a complete coherence contract with live mappings
- see `vmem.md` for full mapping, unmapping, sharing, and file-offset rules

### Console, Keyboard, and VGA Helpers

These traps expose device-oriented helpers rather than POSIX-style syscalls.
MMIO register behavior and pixel/tile formats come from `../../docs/mem_map.md`.

| Code | Wrapper | Arguments | Result |
| --- | --- | --- | --- |
| `3` | `getkey()` | none | Returns the next queued keyboard event, or `0` if no key is pending. Unlike `read(STDIN, ...)`, this trap does not block. |
| `4` | `set_tile_scale(scale)` | `scale` | Writes the tile-scale register and returns `0`. |
| `5` | `set_vscroll(value)` | `value` | Writes the tile vertical-scroll register and returns `0`. |
| `6` | `set_hscroll(value)` | `value` | Writes the tile horizontal-scroll register and returns `0`. |
| `7` | `load_text_tiles()` | none | Loads the built-in text tileset and returns `0`. |
| `8` | `clear_screen()` | none | Clears the display using the kernel VGA helper and returns `0`. |
| `9` | `get_tilemap()` | none | Maps the tilemap MMIO region into user space and returns the user pointer, or `(short*)-1` if no virtual range is available. |
| `10` | `get_tile_fb()` | none | Maps the tile framebuffer MMIO region into user space and returns the user pointer, or `(short*)-1` if no virtual range is available. |
| `11` | `get_vga_status()` | none | Returns the low byte of the VGA status register. |
| `12` | `get_vga_frame_counter()` | none | Returns the 32-bit VGA frame counter. |
| `26` | `set_text_color(color)` | `color` | Updates the console text color used by formatted output and returns `0`. |
| `36` | `move_vscroll(delta)` | `delta` | Adds `delta` to the tile vertical-scroll register and returns `0`. |
| `37` | `move_hscroll(delta)` | `delta` | Adds `delta` to the tile horizontal-scroll register and returns `0`. |
| `43` | `set_sprite_scale(sprite_num, scale)` | `sprite_num`, `scale` | Writes one sprite-scale register and returns `0`, or returns `-1` for an invalid sprite number. |
| `44` | `set_sprite_coords(sprite_num, x, y)` | `sprite_num`, `x`, `y` | Writes one sprite coordinate pair and returns `0`, or returns `-1` for an invalid sprite number. |
| `45` | `load_text_tiles_colored(fg_color, bg_color)` | `fg_color`, `bg_color` | Loads the built-in text tileset with explicit colors, clears the display, and returns `0`. |
| `46` | `get_spritemap()` | none | Maps the sprite MMIO region into user space and returns the user pointer, or `(short*)-1` if no virtual range is available. |

The three display mapping getters are idempotent within one process: repeating
an exact getter returns the same pointer and does not reserve another virtual
range. A child inherits an existing display mapping at the same virtual
address. Parent and child mappings alias the same live physical/MMIO pages,
including pages touched before `fork()`; they are not private snapshots.

The mappings are page-granular, but callers must access only the device byte
ranges and formats defined in `../../docs/mem_map.md`. Access to rounded
trailing bytes without a documented device meaning is unspecified. These
direct mappings are borrowed and are never returned to the physical-page
allocator during process teardown.
