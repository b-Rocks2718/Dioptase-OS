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
- `execv()` argument count: at most 16
- each `execv()` argument string: at most 256 bytes including the terminating
  NUL
- rebuilt `execv()` argv block must fit in the initial 16 KiB user stack

### Process, Time, and Scheduling

| Code | Wrapper | Arguments | Result |
| --- | --- | --- | --- |
| `0` | `exit(status)` | `status` | Terminates the current user image. On success this trap does not return to user mode; `wait_child()` later returns the child's exit status. |
| `1` | `test_syscall(arg)` | `arg` | Test-only trap. Emits `***test_syscall arg = <arg>` and returns `arg + 7`. |
| `2` | `get_current_jiffies()` | none | Returns the current global jiffy counter. |
| `13` | `sleep(jiffies)` | `jiffies` | Blocks the caller for at least the requested number of jiffies, then returns `0`. |
| `23` | `fork()` | none | Returns `0` in the child. Returns a child descriptor in `200..299` in the parent. Returns `-1` on failure. The child inherits the parent's cwd, descriptor tables, and user address space snapshot. |
| `24` | `execv(path, argc, argv)` | `path`, `argc`, `argv` | Replaces the current user image. Success does not return. Failure returns `-1`. If `argc == 0`, `argv` is ignored and the new image starts with `argc = 0`, `argv = NULL`. |
| `27` | `wait_child(child_desc)` | `child_desc` | Blocks until the specified child exits, returns that child's exit status, then consumes the child descriptor. Re-waiting the same descriptor returns `-1`. |
| `32` | `yield()` | none | Voluntarily yields the CPU and returns `0`. |

### Exec

- `execv()` snapshots the user `argv[]` array and every argument string before
  destroying the old address space.
- The new image starts with `argc` in `r1` and the rebuilt `argv` pointer in
  `r2`, matching the normal function-call ABI for `main(int argc, char** argv)`.
- The rebuilt `argv[]` contains a trailing `NULL` entry.

For `argc > 0`, `argv` must point to a readable user array of `argc` readable
NUL-terminated strings. Invalid pointers, unterminated strings, or oversized
argument vectors return `-1`.

### Filesystem, Pipes, and Audio

| Code | Wrapper | Arguments | Result |
| --- | --- | --- | --- |
| `14` | `open(path)` | `path` | Resolves `path` from the current cwd unless the path is absolute. Returns a file descriptor in `0..99`, or `-1` on copy, lookup, or descriptor-allocation failure. |
| `15` | `read(fd, buf, count)` | `fd`, `buf`, `count` | Copies up to the clamped byte count into `buf`. Returns the number of bytes read, `0` at EOF, or `-1` on failure. `STDIN` reads block waiting for keyboard input. |
| `16` | `write(fd, buf, count)` | `fd`, `buf`, `count` | Copies up to the clamped byte count from `buf`. Returns the number of bytes written or `-1` on failure. `STDOUT` and `STDERR` write characters to the console. |
| `17` | `close(fd)` | `fd` | Closes a valid file descriptor and returns `0`, or returns `-1` for an invalid descriptor. |
| `25` | `play_audio_file(fd)` | `fd` | Starts asynchronous playback of a regular file and returns `0`, or returns `-1` if `fd` is invalid or does not name a regular file. |
| `28` | `chdir(path)` | `path` | Resolves `path` relative to the current cwd unless absolute, requires the result to be a directory, updates the cwd, and returns `0`. Returns `-1` on copy or lookup failure, or if the target is not a directory. |
| `29` | `pipe(fds)` | `fds` | Allocates a pipe and writes `{read_fd, write_fd}` into the user array `fds[0..1]`. Returns `0` on success or `-1` on copy or descriptor-allocation failure. |
| `30` | `dup(fd)` | `fd` | Returns a new file descriptor that references the same underlying descriptor object, including the shared current offset. Returns `-1` on failure. |
| `31` | `seek(fd, offset, whence)` | `fd`, `offset`, `whence` | Updates the descriptor offset and returns the new offset, or returns `-1` for an invalid descriptor or invalid `whence`. |

Additional file-descriptor notes:
- `dup()` shares the same underlying descriptor object, so offset changes are
  visible through both the original descriptor and the duplicate.
- `seek()` uses `SEEK_SET = 0`, `SEEK_CUR = 1`, and `SEEK_END = 2`.
- `SEEK_SET` rejects negative offsets.
- `SEEK_CUR` rejects results that would be negative or would exceed signed
  32-bit range.
- `SEEK_END` rejects results that would be negative or would exceed signed
  32-bit range.
- Current implementation detail: `play_audio_file()` spawns a playback worker
  and returns immediately. The worker currently expects a supported PCM WAV
  file; malformed WAV contents currently panic the kernel instead of returning
  `-1`.

### Synchronization and Virtual Memory

| Code | Wrapper | Arguments | Result |
| --- | --- | --- | --- |
| `18` | `sem_open(count)` | `count` | Allocates a semaphore descriptor in `100..199`, initializes it to `count`, and returns that descriptor. Returns `-1` for negative counts or exhaustion. |
| `19` | `sem_up(sem)` | `sem` | Increments the semaphore and returns `0`, or returns `-1` for an invalid semaphore descriptor. |
| `20` | `sem_down(sem)` | `sem` | Decrements the semaphore, blocking if needed, and returns `0`, or returns `-1` for an invalid semaphore descriptor. |
| `21` | `sem_close(sem)` | `sem` | Closes a semaphore descriptor and returns `0`, or returns `-1` for an invalid semaphore descriptor. |
| `22` | `mmap(size, fd, offset, flags)` | `size`, `fd`, `offset`, `flags` | Returns the base address of the new user mapping, or `-1` on failure. `fd == MMAP_ANON` requests an anonymous mapping. |

`mmap()` details that matter to user mode:

- user-visible flags come from `crt/sys.h`: `MMAP_SHARED`, `MMAP_READ`,
  `MMAP_WRITE`, and `MMAP_EXEC`
- the kernel always adds the internal `MMAP_USER` bit to user-created mappings
- anonymous shared mappings are currently rejected
- file-backed mappings require a valid file descriptor and a non-negative offset
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
| `9` | `get_tilemap()` | none | Maps the tilemap MMIO region into user space and returns the user pointer. |
| `10` | `get_tile_fb()` | none | Maps the tile framebuffer MMIO region into user space and returns the user pointer. |
| `11` | `get_vga_status()` | none | Returns the low byte of the VGA status register. |
| `12` | `get_vga_frame_counter()` | none | Returns the 32-bit VGA frame counter. |
| `26` | `set_text_color(color)` | `color` | Updates the console text color used by formatted output and returns `0`. |
