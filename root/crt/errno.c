/*
 * Bootstrap userland currently does not thread errno through every syscall
 * wrapper yet. Keep a process-global storage cell so code that references the
 * standard symbol still links while the richer error plumbing is added later.
 */

int errno = 0;
