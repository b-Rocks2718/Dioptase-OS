## Syscalls

User programs enter the kernel with `trap` using the trap ABI documented in
`../../docs/abi.md`.

### User Pointers

Syscalls that accept user pointers validate the complete user-side range before
copying. A valid user range must:

- live in the user virtual address range
- be covered by user VMEs in the current thread
- have read permission for kernel copies from user memory
- have write permission for kernel copies to user memory

Invalid user pointers return `-1`. They must not panic the kernel and must not
copy through low kernel/physical aliases.

Path arguments are copied as bounded NUL-terminated strings. The current maximum
path argument size is 1024 bytes including the terminating NUL; longer or
unterminated paths return `-1`.

### Current File Syscalls

- `open(path)`: returns a file descriptor for a path relative to the current
  working directory, or `-1` on lookup/copy/fd allocation failure.
- `read(fd, buf, count)`: copies up to `count` bytes into `buf`, returning the
  number of bytes copied, `0` at EOF, or `-1` on failure.
- `write(fd, buf, count)`: copies up to `count` bytes from `buf`, returning the
  number of bytes written or `-1` on failure.
- `close(fd)`: closes a valid file descriptor and returns `0`, or returns `-1`
  for an invalid descriptor.

`read` and `write` currently clamp each call to at most 1024 bytes. This limit is
implementation-defined and not an ISA requirement.
