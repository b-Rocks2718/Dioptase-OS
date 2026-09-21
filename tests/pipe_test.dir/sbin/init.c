/*
 * Pipe delivery and endpoint-lifecycle regression.
 *
 * Validates:
 * - a rejected user destination does not consume queued bytes
 * - dup() references keep one logical writer open until its final close
 * - buffered bytes drain before EOF after the final writer closes
 * - a nonempty write with no readers returns -1
 * - final-reader close wakes a full-buffer writer, which returns its already
 *   committed positive prefix rather than hanging or losing that prefix
 *
 * The fork case preloads 1,023 of the 1,024 slots. The child drops its inherited
 * read reference and writes two bytes. The parent waits until the observable
 * queue size is exactly 1,024, proving the first byte was committed, before it
 * closes the final read reference. No scheduler timing assumption is needed.
 */

#include "../../../root/crt/sys.h"
#include "../../user_test.h"

#define BAD_LOW_POINTER ((char*)0x1000)
#define DELIVERY_BYTES 3
#define DRAIN_BUFFER_BYTES 8
#define PIPE_CAPACITY_BYTES 1024
// Leave exactly one slot free so the two-byte child write has a known prefix.
#define PIPE_PRELOAD_BYTES 1023
#define BLOCKED_WRITE_BYTES 2

#define CHILD_STATUS_OK 61
#define CHILD_STATUS_CLOSE_READ_FAILED 62
#define CHILD_STATUS_READY_FAILED 63
#define CHILD_STATUS_PARTIAL_WRITE_FAILED 64
#define CHILD_STATUS_CLOSE_WRITE_FAILED 65

static int bytes_equal(char* lhs, char* rhs, unsigned count){ /* Compare two byte ranges for exact equality. */
  for (unsigned i = 0; i < count; ++i){
    if (lhs[i] != rhs[i]){
      return 0;
    }
  }
  return 1;
}

static int blocked_writer_child(int read_fd, int write_fd, int ready_sem){ /* Run the blocked writer child process. */
  char suffix[BLOCKED_WRITE_BYTES];
  suffix[0] = 'X';
  suffix[1] = 'Y';

  // The parent retains the other table reference to this shared read endpoint
  // object, so this close alone must not publish final-reader closure.
  if (close(read_fd) != 0){
    return CHILD_STATUS_CLOSE_READ_FAILED;
  }
  if (sem_up(ready_sem) != 0){
    return CHILD_STATUS_READY_FAILED;
  }

  // One byte fills the final slot. The second blocks until the parent closes
  // its final read reference, at which point write() must return the prefix 1.
  if (write(write_fd, suffix, BLOCKED_WRITE_BYTES) != 1){
    return CHILD_STATUS_PARTIAL_WRITE_FAILED;
  }
  if (close(write_fd) != 0){
    return CHILD_STATUS_CLOSE_WRITE_FAILED;
  }
  return CHILD_STATUS_OK;
}

int main(void){ /* Verify pipe reads, writes, closure, and blocking behavior. */
  int fds[2] = {-1, -1};
  char payload[DELIVERY_BYTES];
  char drain[DRAIN_BUFFER_BYTES];
  char preload[PIPE_PRELOAD_BYTES];
  payload[0] = 'A';
  payload[1] = 'B';
  payload[2] = 'C';

  int rc = pipe(fds);
  user_test_expect_eq("create delivery pipe", rc, 0);
  if (rc != 0){
    return 1;
  }

  int duplicate_writer = dup(fds[1]);
  user_test_expect_eq("duplicate delivery writer is valid",
    duplicate_writer >= 0, 1);
  user_test_expect_eq("write delivery payload",
    write(fds[1], payload, DELIVERY_BYTES), DELIVERY_BYTES);
  user_test_expect_eq("invalid pipe read destination",
    read(fds[0], BAD_LOW_POINTER, 1), -1);
  user_test_expect_eq("invalid destination preserved all queued bytes",
    fd_bytes_available(fds[0]), DELIVERY_BYTES);

  user_test_expect_eq("close original delivery writer", close(fds[1]), 0);
  user_test_expect_eq("duplicate kept delivery writer open",
    fd_bytes_available(fds[0]), DELIVERY_BYTES);
  user_test_expect_eq("close final delivery writer", close(duplicate_writer),
    0);

  int bytes_read = read(fds[0], drain, DRAIN_BUFFER_BYTES);
  user_test_expect_eq("drain buffered bytes after final writer close",
    bytes_read, DELIVERY_BYTES);
  user_test_expect_eq("drained payload matches",
    bytes_equal(drain, payload, DELIVERY_BYTES), 1);
  user_test_expect_eq("read EOF after buffered bytes drain",
    read(fds[0], drain, 1), 0);
  user_test_expect_eq("close drained read endpoint", close(fds[0]), 0);

  rc = pipe(fds);
  user_test_expect_eq("create broken-reader pipe", rc, 0);
  if (rc != 0){
    return 2;
  }
  user_test_expect_eq("close only broken-reader endpoint", close(fds[0]), 0);
  user_test_expect_eq("write with no readers",
    write(fds[1], payload, 1), -1);
  user_test_expect_eq("close broken-reader writer", close(fds[1]), 0);

  rc = pipe(fds);
  user_test_expect_eq("create blocked-writer pipe", rc, 0);
  if (rc != 0){
    return 3;
  }

  for (unsigned i = 0; i < PIPE_PRELOAD_BYTES; ++i){
    preload[i] = (char)i;
  }
  user_test_expect_eq("preload all but one pipe slot",
    write(fds[1], preload, PIPE_PRELOAD_BYTES), PIPE_PRELOAD_BYTES);

  int ready_sem = sem_open(0);
  user_test_expect_eq("open blocked-writer ready semaphore",
    ready_sem >= 100, 1);
  int child = fork();
  if (child == 0){
    return blocked_writer_child(fds[0], fds[1], ready_sem);
  }

  user_test_expect_eq("blocked-writer child descriptor is valid",
    child >= 200, 1);
  user_test_expect_eq("close parent blocked-writer reference",
    close(fds[1]), 0);
  user_test_expect_eq("blocked-writer child reached write",
    sem_down(ready_sem), 0);

  // Reaching capacity proves the child committed its first byte. The next
  // loop iteration cannot commit while this parent retains the only reader.
  while (fd_bytes_available(fds[0]) != PIPE_CAPACITY_BYTES){
    yield();
  }
  user_test_expect_eq("blocked writer committed first byte",
    fd_bytes_available(fds[0]), PIPE_CAPACITY_BYTES);
  user_test_expect_eq("close final reader wakes blocked writer",
    close(fds[0]), 0);
  user_test_expect_eq("blocked writer returned committed prefix",
    wait_child(child), CHILD_STATUS_OK);
  user_test_expect_eq("close blocked-writer ready semaphore",
    sem_close(ready_sem), 0);

  return 0;
}
