#include "stdio.h"

#include "fcntl.h"
#include "print.h"
#include "stdlib.h"

/*
 * Purpose: Provide the tiny FILE shim used by bootstrap userland tools.
 * Inputs: Streams are represented as storage for one Dioptase fd.
 * Outputs: fopen/fclose/fputc/fputs/fwrite behave on those fd-backed streams.
 * Invariants/Assumptions: FILE remains a single int and unistd.h/fcntl.h own
 * the fd constants plus open/read/write/seek/truncate semantics.
 */

FILE __stdin_storage = STDIN;
FILE __stdout_storage = STDOUT;
FILE __stderr_storage = STDERR;

static int stream_fd(FILE* stream) {
  if (stream == NULL) {
    return -1;
  }
  return *stream;
}

static int is_std_stream(FILE* stream) {
  return stream == stdin || stream == stdout || stream == stderr;
}

// Purpose: Open one Dioptase path and wrap its fd in FILE storage.
// Inputs: path names the file; mode only distinguishes write-truncate vs read.
// Outputs: Returns a heap-allocated FILE wrapper or NULL on failure.
// Invariants/Assumptions: fcntl.h exposes open() without flags, so the mode
// string can only request truncation/rewind after a successful open.
FILE* fopen(char* path, char* mode) {
  int fd;
  FILE* stream;

  fd = open(path);
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

// Purpose: Close one FILE shim stream.
// Inputs: stream may be a heap stream or one of stdin/stdout/stderr.
// Outputs: Returns the underlying close result or 0 for standard streams.
// Invariants/Assumptions: Standard stream storage is static and must not be freed.
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

// Purpose: Write one byte to a FILE shim stream.
// Inputs: c is converted to one byte; stream must reference a writable fd.
// Outputs: Returns the byte value on success or -1 on write failure.
// Invariants/Assumptions: write() returns 1 for one successful byte.
int fputc(int c, FILE* stream) {
  char ch;

  ch = (char)c;
  if (write(stream_fd(stream), &ch, 1) != 1) {
    return -1;
  }
  return (unsigned char)ch;
}

// Purpose: Write one NUL-terminated string to a FILE shim stream.
// Inputs: str may be NULL; stream must reference a writable fd.
// Outputs: Returns the fdputs result or -1 for NULL input.
// Invariants/Assumptions: print.h fdputs already handles ordinary string output.
int fputs(char* str, FILE* stream) {
  if (str == NULL) {
    return -1;
  }
  return (int)fdputs(stream_fd(stream), str);
}

// Purpose: Read raw bytes from a FILE shim stream.
// Inputs: ptr points to size*count writable bytes; stream must reference a readable fd.
// Outputs: Returns the number of whole items read before EOF or read failure.
// Invariants/Assumptions: The syscall layer may short-read, so this helper
// loops until the request completes or read() stops making progress.
size_t fread(void* ptr, size_t size, size_t count, FILE* stream) {
  char* bytes;
  size_t total;
  size_t read_total;

  if (ptr == NULL || stream == NULL) {
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

  if (size == 0) {
    return 0;
  }
  return read_total / size;
}

// Purpose: Write raw bytes to a FILE shim stream.
// Inputs: ptr points to size*count bytes; stream must reference a writable fd.
// Outputs: Returns the number of whole items written before any short write.
// Invariants/Assumptions: The syscall layer may short-write, so this helper
// loops until the transfer completes or write() stops making progress.
size_t fwrite(void* ptr, size_t size, size_t count, FILE* stream) {
  char* bytes;
  size_t total;
  size_t written;

  if (ptr == NULL || stream == NULL) {
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

  if (size == 0) {
    return 0;
  }
  return written / size;
}

// Purpose: Reposition one FILE shim stream.
// Inputs: offset/whence follow the Dioptase seek() syscall contract.
// Outputs: Returns 0 on success or -1 on failure.
// Invariants/Assumptions: seek() returns the new offset, mirroring sys.h.
int fseek(FILE* stream, int offset, int whence) {
  if (stream == NULL) {
    return -1;
  }
  if (seek(stream_fd(stream), offset, whence) < 0) {
    return -1;
  }
  return 0;
}

// Purpose: Report the current offset of one FILE shim stream.
// Inputs: stream must reference an open seekable fd.
// Outputs: Returns the current offset, or -1 on failure.
// Invariants/Assumptions: seek(fd, 0, SEEK_CUR) exposes the current position.
int ftell(FILE* stream) {
  if (stream == NULL) {
    return -1;
  }
  return seek(stream_fd(stream), 0, SEEK_CUR);
}
