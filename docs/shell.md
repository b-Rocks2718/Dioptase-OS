# Shell

This document describes the current `/sbin/shell` implementation in
`root/shell/shell.c`.

Dioptase-OS does not currently define a POSIX-compatible shell language. This
is a small interactive command interpreter with a fixed set of built-ins and a
simple external-command launcher.

Related documents:
- `syscalls.md` for `fork()`, `execv()`, `wait_child()`, `getkey()`, path
  resolution, and file operations
- `filesystem.md` for pathname traversal and current ext2 behavior
- `terminal.md` for the ANSI sequences that the shell prompt and `clear`
  command rely on

## Startup and Process Topology

- The kernel runs `/sbin/init` as the first user program.
- `/sbin/init` starts `/sbin/terminal`, then starts `/sbin/shell`.
- The shell writes prompts and command output to `STDOUT`, which the terminal
  process renders.
- The shell reads keyboard events itself with `getkey()`. It does not use
  `read(STDIN, ...)` for line editing.
- When the shell exits, `/sbin/init` waits for it, kills the terminal process,
  and currently returns status `67`.

## Input Model

The shell keeps one in-memory command buffer and edits it interactively before
running a command.

Current implementation-defined limits and behaviors:

- Command buffer size: `2048` bytes
- The current editor accepts at most `2047` typed characters before ignoring
  further printable input
- Accepted editing keys: printable ASCII, Enter, Backspace, left Shift, and
  right Shift
- Characters outside printable ASCII are ignored unless they are one of the
  special keys above
- Backspace deletes one buffered character and visually erases it with
  `"\b \b"`
- There is no cursor motion, insert mode, command history, or tab completion
- The shell sleeps for `5` jiffies between keyboard polls when waiting for
  input

## Command Parsing

Command parsing is intentionally minimal.

- The shell splits the command line on the literal space character `' '`
- Multiple adjacent spaces are collapsed
- Leading spaces are ignored
- Maximum argument count: `16` total `argv[]` entries, including `argv[0]`
- Tokens beyond the first `16` are silently ignored
- There is no quoting or escaping, so arguments cannot contain spaces
- There is no syntax for pipes, redirection, command substitution, globbing,
  variable expansion, or command chaining

Built-in command matching is case-sensitive.

## Built-In Commands

### `cd PATH`

- Calls `chdir(PATH)`
- Relative and absolute paths follow the normal pathname traversal rules from
  `syscalls.md` and `filesystem.md`
- On failure, prints `cd: failed to change directory`

### `ls`

- Lists entries from the current directory only
- Ignores any extra arguments
- Reads one `1024`-byte `getdents()` chunk and prints each returned entry on
  its own line
- Directories are printed in blue and non-directories in white

Current caveats:

- Large directories may be truncated because the shell does not loop over
  multiple `getdents()` calls

### `cat FILE`

- Opens `FILE`
- Reads it in `1024`-byte chunks
- Writes the bytes directly to `STDOUT`

Current caveat:

- `open()` in Dioptase-OS creates a file if it does not exist, so `cat` does
  not currently report a missing file the way a Unix user might expect. A
  missing path may instead create an empty file and print nothing.

### `cp SRC DEST`

- Opens `SRC`
- Opens `DEST`
- Copies data in `1024`-byte chunks from source to destination
- Truncates `DEST` to the final source size after copying

Current caveats:

- `DEST` creation follows the current `open()` contract, so a missing
  destination file is created automatically
- Because `open()` also creates a missing source path, `cp` does not currently
  fail cleanly for a missing source file. It may create an empty source file
  and then copy zero bytes
- This is a byte-stream copy only. There is no metadata preservation, recursive
  copy, or option parsing

### `mv SRC DEST`

- Implemented as `cp SRC DEST` followed by `unlink(SRC)`
- There is no dedicated rename syscall in the shell implementation

Current caveats:

- This is not atomic
- Because it ends with `unlink(SRC)`, it only works for non-directory sources
- The shell currently does not check whether `cp` succeeded before attempting
  `unlink(SRC)`, so a partial or failed copy can still be followed by source
  removal
- Missing-source behavior inherits the current `cp` caveats described above

### `mkdir NAME`

- Calls `mkdir(NAME)`
- On success, creates one empty directory entry in the current working
  directory

Current caveat:

- The current syscall contract is basename-only for `mkdir()`, so `NAME` must
  be one non-empty path component in the current directory. Slash-separated and
  absolute paths fail.

### `rm NAME`

- Calls `unlink(NAME)`
- Removes one non-directory entry

Current caveat:

- The current syscall contract is basename-only for `unlink()`, so `NAME` must
  be one non-empty path component in the current directory. Slash-separated and
  absolute paths fail.

### `rmdir NAME`

- Calls `rmdir(NAME)`
- Removes one empty directory

Current caveat:

- The current syscall contract is basename-only for `rmdir()`, so `NAME` must
  be one non-empty path component in the current directory. Slash-separated and
  absolute paths fail.

### `clear`

- Prints `ESC [ 2 J` followed by `ESC [ H`
- This relies on the terminal behavior documented in `terminal.md`

### `help`

- Prints a short summary of the built-in commands

### `exit`

- Calls `exit(0)`
- This terminates the shell process and ends the current interactive shell
  session started by `/sbin/init`

## External Command Execution

If a command name does not match a built-in, the shell launches an external
program.

- The shell calls `fork()`
- The child first tries `execv("/sbin/<argv0>", argc, argv)`
- If that fails, the child tries `execv(argv0, argc, argv)`
- If both `execv()` calls fail, the child prints `failed to exec command` and
  exits with status `1`
- The parent waits synchronously with `wait_child()`

Consequences:

- There is no `PATH` search beyond the fixed `/sbin/` attempt and the literal
  command name
- There are no background jobs or job-control commands
- Absolute paths and slash-containing relative paths are still attempted
  literally because the shell falls back to `argv[0]` after the `/sbin/` try

## Unsupported Features

The current shell does not implement:

- quoting or escaping
- pipes or redirection
- environment variables or variable expansion
- wildcard expansion
- command separators such as `;`, `&&`, or `||`
- subshells
- command history
- tab completion
- job control
