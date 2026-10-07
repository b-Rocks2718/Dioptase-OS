#ifndef ERRNO_H
#define ERRNO_H

// Syscall failure causes. These numbers are user-visible ABI and must match
// kernel/syscall_errors.h; docs/syscalls.md ("Error Reporting") lists which
// syscalls report which codes. A syscall that fails without reporting a cause
// leaves errno unchanged.
#define ENOENT 2        // the path did not resolve to an existing inode
#define EFAULT 14       // a user pointer argument was not readable/writable
#define ENOTDIR 20      // a non-final path component is not a directory
#define EMFILE 24       // the file-descriptor table is full
#define ENAMETOOLONG 36 // a path or path component exceeds its byte limit

// Most recent nonzero syscall failure cause. The crt syscall wrappers set it
// only when the kernel reports a cause; nothing ever clears it, so callers
// that need to know whether a specific call reported one set errno = 0 first.
// User processes are single-threaded, so one process-global cell suffices.
extern int errno;

#endif // ERRNO_H
