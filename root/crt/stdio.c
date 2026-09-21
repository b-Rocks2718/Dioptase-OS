#include "stdio.h"

#include "fcntl.h"
#include "limits.h"
#include "print.h"
#include "stdlib.h"

/*
 * Provide the tiny FILE shim used by bootstrap userland tools.
 * Fopen/fclose/fputc/fputs/fwrite behave on those fd-backed streams.
 * FILE remains a single int and unistd.h/fcntl.h own
 * the fd constants plus open/read/write/seek/truncate semantics.
 */

FILE __stdin_storage = STDIN;
FILE __stdout_storage = STDOUT;
FILE __stderr_storage = STDERR;

// Return the file descriptor backing a standard stream.
static int stream_fd(FILE* stream) {
  if (stream == NULL) {
    return -1;
  }
  return *stream;
}

// Return whether the stream is one of stdin, stdout, or stderr.
static int is_std_stream(FILE* stream) {
  return stream == stdin || stream == stdout || stream == stderr;
}

// Open one Dioptase path and wrap its fd in FILE storage.
// Returns a heap-allocated FILE wrapper or NULL on failure.
// A leading `w` intentionally uses creating open(),
// then truncates and rewinds. Every other mode is this CRT's read behavior and
// uses open_existing(), so a missing input cannot be created as a side effect.
FILE* fopen(char* path, char* mode) {
  int fd;
  FILE* stream;

  if (mode != NULL && mode[0] == 'w') {
    fd = open(path);
  } else {
    fd = open_existing(path);
  }
  if (fd < 0) {
    return NULL;
  }

  if (mode != NULL && mode[0] == 'w') {
    if (truncate(fd, 0) < 0) {
      close(fd);
      return NULL;
    }
    if (seek(fd, 0, SEEK_SET) < 0) {
      close(fd);
      return NULL;
    }
  }

  stream = malloc(sizeof(FILE));
  if (stream == NULL) {
    close(fd);
    return NULL;
  }
  *stream = fd;
  return stream;
}

// Close one FILE shim stream.
// Stream may be a heap stream or one of stdin/stdout/stderr.
// Returns the underlying close result or 0 for standard streams.
// Standard stream storage is static and must not be freed.
int fclose(FILE* stream) {
  int fd;
  int result;

  if (stream == NULL) {
    return -1;
  }
  if (is_std_stream(stream)) {
    return 0;
  }

  fd = *stream;
  result = close(fd);
  free(stream);
  return result;
}

// Write one byte to a FILE shim stream.
// C is converted to one byte; stream must reference a writable fd.
// Returns the byte value on success or -1 on write failure.
int fputc(int c, FILE* stream) {
  char ch;

  ch = (char)c;
  if (write(stream_fd(stream), &ch, 1) != 1) {
    return -1;
  }
  return (unsigned char)ch;
}

// Write one NUL-terminated string to a FILE shim stream.
// Str may be NULL; stream must reference a writable fd.
// Returns the fdputs result or -1 for NULL input.
int fputs(char* str, FILE* stream) {
  if (str == NULL) {
    return -1;
  }
  return (int)fdputs(stream_fd(stream), str);
}

// Read raw bytes from a FILE shim stream.
// Ptr points to size*count writable bytes; stream must reference a readable fd.
// Returns the number of whole items read before EOF or read failure.
// The syscall layer may short-read, so this helper
// loops until the request completes or read() stops making progress. An item
// extent that is not representable by the 32-bit size_t contract is rejected
// before pointer arithmetic or descriptor I/O.
size_t fread(void* ptr, size_t size, size_t count, FILE* stream) {
  char* bytes;
  size_t total;
  size_t read_total;

  if (ptr == NULL || stream == NULL || size == 0 || count == 0) {
    return 0;
  }
  if (count > UINT_MAX / size) {
    return 0;
  }

  bytes = (char*)ptr;
  total = size * count;
  read_total = 0;
  while (read_total < total) {
    int rc = read(stream_fd(stream), bytes + read_total, (unsigned)(total - read_total));
    if (rc <= 0) {
      break;
    }
    read_total += (size_t)rc;
  }

  return read_total / size;
}

// Write raw bytes to a FILE shim stream.
// Ptr points to size*count bytes; stream must reference a writable fd.
// Returns the number of whole items written before any short write.
// The syscall layer may short-write, so this helper
// loops until the transfer completes or write() stops making progress. An item
// extent that is not representable by the 32-bit size_t contract is rejected
// before pointer arithmetic or descriptor I/O.
size_t fwrite(void* ptr, size_t size, size_t count, FILE* stream) {
  char* bytes;
  size_t total;
  size_t written;

  if (ptr == NULL || stream == NULL || size == 0 || count == 0) {
    return 0;
  }
  if (count > UINT_MAX / size) {
    return 0;
  }

  bytes = (char*)ptr;
  total = size * count;
  written = 0;
  while (written < total) {
    int rc = write(stream_fd(stream), bytes + written, (unsigned)(total - written));
    if (rc <= 0) {
      break;
    }
    written += (size_t)rc;
  }

  return written / size;
}

// Reposition one FILE shim stream.
// Returns 0 on success or -1 on failure.
int fseek(FILE* stream, int offset, int whence) {
  if (stream == NULL) {
    return -1;
  }
  if (seek(stream_fd(stream), offset, whence) < 0) {
    return -1;
  }
  return 0;
}

// Report the current offset of one FILE shim stream.
// Stream must reference an open seekable fd.
// Returns the current offset, or -1 on failure.
int ftell(FILE* stream) {
  if (stream == NULL) {
    return -1;
  }
  return seek(stream_fd(stream), 0, SEEK_CUR);
}
