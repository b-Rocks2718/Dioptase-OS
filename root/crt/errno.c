/*
 * Process-global errno storage. The syscall wrappers in sys.s store the
 * kernel's nonzero r2 failure cause here after each trap; see errno.h.
 */

int errno = 0;
