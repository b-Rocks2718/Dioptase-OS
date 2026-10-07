#ifndef SYSCALL_ERRORS_H
#define SYSCALL_ERRORS_H

// Syscall failure causes returned to user mode in r2 (see docs/syscalls.md,
// "Error Reporting"). These values are user-visible ABI: root/crt/errno.h must
// define the same numbers. The numbering follows the familiar Unix values so
// ported code reads naturally, but only the codes listed here are defined by
// Dioptase-OS.
//
// 0 means "no cause reported": either the syscall succeeded or the failing
// path has not been converted to report a specific cause yet.
#define SYSCALL_ERROR_NONE 0
#define ENOENT 2        // the path did not resolve to an existing inode
#define EFAULT 14       // a user pointer argument was not readable/writable
#define ENOTDIR 20      // a non-final path component is not a directory
#define EMFILE 24       // the caller's file-descriptor table is full
#define ENAMETOOLONG 36 // a path or path component exceeds its byte limit

#endif // SYSCALL_ERRORS_H
