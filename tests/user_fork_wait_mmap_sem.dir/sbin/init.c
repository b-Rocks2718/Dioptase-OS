/*
 * user_fork_wait_mmap_sem guest:
 * - validate that a semaphore opened before fork is usable from both parent
 *   and child
 * - validate that a pipe created before fork can carry data from the child to
 *   the parent through inherited file descriptors
 * - validate that wait_child returns the child's exit status
 * - validate that a pre-fork private file mapping stays private after fork
 * - validate that a pre-fork shared file mapping stays shared after fork and
 *   remains visible through an ordinary file read
 *
 * How:
 * - open one private fixture file and one shared fixture file
 * - create a pipe before fork so the child inherits both endpoints
 * - mmap both files before fork so the kernel must duplicate the VMEs during
 *   fork rather than building fresh mappings independently in the parent and
 *   child
 * - have the child close its inherited read end, verify the initial bytes,
 *   modify one byte in each mapping, write one byte to the pipe, and wake the
 *   parent by sem_up() on an inherited semaphore descriptor
 * - have the parent close its inherited write end, block in sem_down(), then
 *   verify that it can read the child's pipe byte and that its private mapping
 *   still shows the original file byte while its shared mapping and a fresh
 *   file read both observe the child's shared write
 * - finally wait for the child, verify the returned exit status, and verify
 *   that the same descriptor number cannot be waited on twice
 */

#include "../../../crt/sys.h"

#define PRIVATE_FILE_NAME "private.txt"
#define SHARED_FILE_NAME "shared.txt"

#define MAPPED_BYTES 4

#define PRIVATE_FILE_INITIAL 'p'
#define SHARED_FILE_INITIAL 's'
#define PRIVATE_CHILD_BYTE 'P'
#define SHARED_CHILD_BYTE 'S'
#define PIPE_CHILD_BYTE 'Q'

#define CHILD_STATUS_OK 41
#define CHILD_STATUS_BAD_PRIVATE_INIT 42
#define CHILD_STATUS_BAD_SHARED_INIT 43
#define CHILD_STATUS_BAD_SEM_UP 44
#define CHILD_STATUS_BAD_PIPE_CLOSE 45
#define CHILD_STATUS_BAD_PIPE_WRITE 46

static int mapping_ok(char* mapping){
  return mapping != 0 && (int)mapping != -1;
}

// Re-open the named file so this check does not depend on any inherited file
// descriptor offset state shared across fork().
static int read_first_byte(char* path){
  int fd = open(path);
  char byte;

  if (fd < 0){
    return -1;
  }

  if (read(fd, &byte, 1) != 1){
    close(fd);
    return -1;
  }

  if (close(fd) != 0){
    return -1;
  }

  return (unsigned char)byte;
}

// Run only in the fork child after the inherited mappings and semaphore
// descriptor have been established by the parent setup path.
static int child_main(char* private_map, char* shared_map, int sem,
    int pipe_read_fd, int pipe_write_fd){
  char pipe_byte = PIPE_CHILD_BYTE;

  if (close(pipe_read_fd) != 0){
    return CHILD_STATUS_BAD_PIPE_CLOSE;
  }

  if (private_map[0] != PRIVATE_FILE_INITIAL){
    return CHILD_STATUS_BAD_PRIVATE_INIT;
  }

  if (shared_map[0] != SHARED_FILE_INITIAL){
    return CHILD_STATUS_BAD_SHARED_INIT;
  }

  private_map[0] = PRIVATE_CHILD_BYTE;
  shared_map[0] = SHARED_CHILD_BYTE;

  if (write(pipe_write_fd, &pipe_byte, 1) != 1){
    return CHILD_STATUS_BAD_PIPE_WRITE;
  }

  if (sem_up(sem) != 0){
    return CHILD_STATUS_BAD_SEM_UP;
  }

  return CHILD_STATUS_OK;
}

int main(void){
  int sem = sem_open(0);
  int private_fd = open(PRIVATE_FILE_NAME);
  int shared_fd = open(SHARED_FILE_NAME);
  int pipe_fds[2] = {-1, -1};
  char* private_map = 0;
  char* shared_map = 0;
  char pipe_byte = '\0';

  if (private_fd >= 0){
    private_map = mmap(MAPPED_BYTES, private_fd, 0, MMAP_READ | MMAP_WRITE);
  }
  if (shared_fd >= 0){
    shared_map = mmap(MAPPED_BYTES, shared_fd, 0,
      MMAP_READ | MMAP_WRITE | MMAP_SHARED);
  }

  int pipe_rc = pipe(pipe_fds);

  test_syscall(sem >= 0);
  test_syscall(private_fd >= 0);
  test_syscall(shared_fd >= 0);
  test_syscall(mapping_ok(private_map));
  test_syscall(mapping_ok(shared_map));
  test_syscall(pipe_rc);
  test_syscall(pipe_fds[0] >= 0);
  test_syscall(pipe_fds[1] >= 0);

  if (sem < 0 || private_fd < 0 || shared_fd < 0 ||
      !mapping_ok(private_map) || !mapping_ok(shared_map) ||
      pipe_rc != 0 || pipe_fds[0] < 0 || pipe_fds[1] < 0){
    return 1;
  }

  int child = fork();
  if (child == 0){
    return child_main(private_map, shared_map, sem, pipe_fds[0], pipe_fds[1]);
  }

  test_syscall(close(pipe_fds[1]));
  test_syscall(sem_down(sem));
  test_syscall(read(pipe_fds[0], &pipe_byte, 1));
  test_syscall((unsigned char)pipe_byte);
  test_syscall((unsigned char)private_map[0]);
  test_syscall((unsigned char)shared_map[0]);
  test_syscall(read_first_byte(PRIVATE_FILE_NAME));
  test_syscall(read_first_byte(SHARED_FILE_NAME));
  test_syscall(wait_child(child));
  test_syscall(wait_child(child));
  test_syscall(sem_close(sem));
  test_syscall(close(pipe_fds[0]));
  test_syscall(close(private_fd));
  test_syscall(close(shared_fd));

  return 0;
}
