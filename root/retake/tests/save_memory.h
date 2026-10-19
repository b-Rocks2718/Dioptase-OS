/*
 * Memory-backed guest file operations shared by gameplay and codec tests.
 * Fault knobs model short transfers and partial replacement without touching
 * host files; the production serializer and game wrappers remain unchanged.
 */
/* Two independent guest files, with explicit lengths and descriptor offsets. */
struct TestFile {
  unsigned char bytes[512];
  int length;
  int exists;
  int cursor;
};
static struct TestFile files[2];
static int io_chunk = 1024;
static int write_budget = -1;
static int bad_read = -1;
static int bad_open = -1;
static int bad_close = -1;
static int bad_seek;
static int bad_truncate;
int saveio_errno;

/* Paths must match the two production slots, never a host filesystem path. */
static int file_index(char* path) {
  if (!strcmp(path, "/retake/save0.dat")) return 0;
  if (!strcmp(path, "/retake/save1.dat")) return 1;
  fprintf(stderr, "unexpected save path: %s\n", path);
  abort();
}

/* Creating open preserves previous content and begins at offset zero. */
int saveio_open(char* path) {
  int i = file_index(path);
  if (bad_open == i) return -1;
  files[i].exists = 1;
  files[i].cursor = 0;
  return 3 + i;
}

/* Only a genuine missing path reports ENOENT; injected failures are uncertain. */
int saveio_existing(char* path) {
  int i = file_index(path);
  if (bad_open == i) return -1;
  if (!files[i].exists) { saveio_errno = ENOENT; return -1; }
  files[i].cursor = 0;
  return 3 + i;
}

/* EOF and positive short reads match the guest contract, not POSIX assumptions. */
int saveio_read(int fd, void* dst, unsigned size) {
  int i = fd - 3;
  if (i < 0 || i > 1 || bad_read == i) return -1;
  struct TestFile* f = &files[i];
  int n = (int)size;
  if (n > io_chunk) n = io_chunk;
  if (n > f->length - f->cursor) n = f->length - f->cursor;
  memcpy(dst, f->bytes + f->cursor, (size_t)n);
  f->cursor += n;
  return n;
}

/* Fail after an exact committed prefix to model every possible interruption. */
int saveio_write(int fd, void* src, unsigned size) {
  int i = fd - 3;
  if (i < 0 || i > 1 || write_budget == 0) return -1;
  struct TestFile* f = &files[i];
  int n = (int)size;
  if (n > io_chunk) n = io_chunk;
  if (write_budget >= 0 && n > write_budget) n = write_budget;
  if (f->cursor + n > (int)sizeof(f->bytes)) return -1;
  memcpy(f->bytes + f->cursor, src, (size_t)n);
  f->cursor += n;
  if (f->cursor > f->length) f->length = f->cursor;
  if (write_budget >= 0) write_budget -= n;
  return n;
}

/* Closing may fail independently of whether a complete record reached the file. */
int saveio_close(int fd) { return fd - 3 == bad_close ? -1 : 0; }

/* The production writer requests an explicit beginning-of-file offset only. */
int saveio_seek(int fd, int offset, int whence) {
  if (bad_seek || fd < 3 || fd > 4 || offset != 0 || whence != SEEK_SET) return -1;
  files[fd - 3].cursor = 0;
  return 0;
}

/* Truncation only shrinks, as specified by the Dioptase syscall. */
int saveio_truncate(int fd, unsigned size) {
  if (bad_truncate || fd < 3 || fd > 4 || size > (unsigned)files[fd - 3].length) return -1;
  files[fd - 3].length = (int)size;
  return 0;
}

/* Remove injected failures without changing either slot's stored bytes. */
static void clear_faults(void) {
  io_chunk = 1024;
  write_budget = -1;
  bad_read = -1;
  bad_open = -1;
  bad_close = -1;
  bad_seek = 0;
  bad_truncate = 0;
}

