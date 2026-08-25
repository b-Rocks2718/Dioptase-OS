/*
 * user_fork_wait_mmap_sem guest:
 * - validate that a semaphore opened before fork is usable from both parent
 *   and child
 * - validate that descriptor references keep a semaphore alive when the
 *   parent closes its reference while one child is entering sem_down() and a
 *   second inherited reference later performs sem_up()
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

#include "../../../root/crt/sys.h"
#include "../../user_test.h"

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

#define LIFETIME_WAITER_STATUS_OK 51
#define LIFETIME_WAITER_STATUS_BAD_READY 52
#define LIFETIME_WAITER_STATUS_BAD_DOWN 53
#define LIFETIME_WAKER_STATUS_OK 54
#define LIFETIME_WAKER_STATUS_BAD_TRIGGER 55
#define LIFETIME_WAKER_STATUS_BAD_UP 56

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

// Publish that this child is about to enter target_sem, then wait for the
// permit supplied by the other child. The inherited descriptor reference must
// retain the Semaphore storage after the parent closes its own descriptor.
static int lifetime_waiter_main(int target_sem, int ready_sem){
  if (sem_up(ready_sem) != 0){
    return LIFETIME_WAITER_STATUS_BAD_READY;
  }
  if (sem_down(target_sem) != 0){
    return LIFETIME_WAITER_STATUS_BAD_DOWN;
  }
  return LIFETIME_WAITER_STATUS_OK;
}

// Remain blocked until the parent has closed its target descriptor, then use
// this child's inherited reference to wake the waiter normally.
static int lifetime_waker_main(int target_sem, int trigger_sem){
  if (sem_down(trigger_sem) != 0){
    return LIFETIME_WAKER_STATUS_BAD_TRIGGER;
  }
  if (sem_up(target_sem) != 0){
    return LIFETIME_WAKER_STATUS_BAD_UP;
  }
  return LIFETIME_WAKER_STATUS_OK;
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

  user_test_expect_eq("open inherited semaphore", sem >= 0, 1);
  user_test_expect_eq("open private-mapping fixture", private_fd >= 0, 1);
  user_test_expect_eq("open shared-mapping fixture", shared_fd >= 0, 1);
  user_test_expect_eq("create private mapping", mapping_ok(private_map), 1);
  user_test_expect_eq("create shared mapping", mapping_ok(shared_map), 1);
  user_test_expect_eq("create inherited pipe", pipe_rc, 0);
  user_test_expect_eq("pipe read descriptor valid", pipe_fds[0] >= 0, 1);
  user_test_expect_eq("pipe write descriptor valid", pipe_fds[1] >= 0, 1);

  if (sem < 0 || private_fd < 0 || shared_fd < 0 ||
      !mapping_ok(private_map) || !mapping_ok(shared_map) ||
      pipe_rc != 0 || pipe_fds[0] < 0 || pipe_fds[1] < 0){
    return 1;
  }

  int child = fork();
  if (child == 0){
    return child_main(private_map, shared_map, sem, pipe_fds[0], pipe_fds[1]);
  }

  user_test_expect_eq("close(pipe_fds[1])", close(pipe_fds[1]), 0);
  user_test_expect_eq("sem_down(sem)", sem_down(sem), 0);
  user_test_expect_eq("read(pipe_fds[0], &pipe_byte, 1)", read(pipe_fds[0], &pipe_byte, 1), 1);
  user_test_expect_eq("pipe byte written by child", (unsigned char)pipe_byte,
    PIPE_CHILD_BYTE);
  user_test_expect_eq("parent private mapping stayed private",
    (unsigned char)private_map[0], PRIVATE_FILE_INITIAL);
  user_test_expect_eq("parent shared mapping saw child write",
    (unsigned char)shared_map[0], SHARED_CHILD_BYTE);
  user_test_expect_eq("private backing file stayed unchanged",
    read_first_byte(PRIVATE_FILE_NAME), PRIVATE_FILE_INITIAL);
  user_test_expect_eq("shared backing file saw child write",
    read_first_byte(SHARED_FILE_NAME), SHARED_CHILD_BYTE);
  user_test_expect_eq("fork child exit status", wait_child(child),
    CHILD_STATUS_OK);
  user_test_expect_eq("second wait rejects consumed child descriptor",
    wait_child(child), -1);
  user_test_expect_eq("sem_close(sem)", sem_close(sem), 0);
  user_test_expect_eq("close(pipe_fds[0])", close(pipe_fds[0]), 0);
  user_test_expect_eq("close(private_fd)", close(private_fd), 0);
  user_test_expect_eq("close(shared_fd)", close(shared_fd), 0);

  // Descriptor lifetime regression: three processes initially own target_sem.
  // Once the waiter has reached its handoff point, give it time to enter the
  // blocking syscall, close the parent's reference, and let the other child
  // perform the wake. Even if scheduling delays the actual enqueue, the child
  // descriptor references must retain the object throughout both operations.
  int lifetime_sem = sem_open(0);
  int lifetime_ready_sem = sem_open(0);
  int lifetime_trigger_sem = sem_open(0);
  user_test_expect_eq("open descriptor-lifetime target semaphore",
    lifetime_sem >= 0, 1);
  user_test_expect_eq("open descriptor-lifetime ready semaphore",
    lifetime_ready_sem >= 0, 1);
  user_test_expect_eq("open descriptor-lifetime trigger semaphore",
    lifetime_trigger_sem >= 0, 1);

  if (lifetime_sem < 0 || lifetime_ready_sem < 0 ||
      lifetime_trigger_sem < 0){
    return 1;
  }

  int lifetime_waiter = fork();
  if (lifetime_waiter == 0){
    return lifetime_waiter_main(lifetime_sem, lifetime_ready_sem);
  }
  int lifetime_waker = fork();
  if (lifetime_waker == 0){
    return lifetime_waker_main(lifetime_sem, lifetime_trigger_sem);
  }

  user_test_expect_eq("descriptor-lifetime waiter child valid",
    lifetime_waiter >= 0, 1);
  user_test_expect_eq("descriptor-lifetime waker child valid",
    lifetime_waker >= 0, 1);
  if (lifetime_waiter < 0 || lifetime_waker < 0){
    return 1;
  }

  user_test_expect_eq("waiter reached descriptor-lifetime semaphore",
    sem_down(lifetime_ready_sem), 0);
  sleep(2);
  user_test_expect_eq("parent closes active inherited semaphore",
    sem_close(lifetime_sem), 0);
  user_test_expect_eq("release inherited semaphore waker",
    sem_up(lifetime_trigger_sem), 0);
  user_test_expect_eq("descriptor-lifetime waiter exit status",
    wait_child(lifetime_waiter), LIFETIME_WAITER_STATUS_OK);
  user_test_expect_eq("descriptor-lifetime waker exit status",
    wait_child(lifetime_waker), LIFETIME_WAKER_STATUS_OK);
  user_test_expect_eq("close descriptor-lifetime ready semaphore",
    sem_close(lifetime_ready_sem), 0);
  user_test_expect_eq("close descriptor-lifetime trigger semaphore",
    sem_close(lifetime_trigger_sem), 0);

  return 0;
}
