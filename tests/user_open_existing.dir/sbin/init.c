/*
 * Exercises the lookup-only open contract through both the raw CRT wrapper and
 * real bundled read paths:
 * - existing files and directories still receive ordinary descriptors
 * - invalid, unterminated, and overlong user paths fail without a kernel panic
 * - repeated missing lookups and read-mode fopen do not create entries
 * - write-mode fopen and legacy open retain intentional creation
 * - shell cat/cp/mv missing sources do not synthesize source or destination
 * - failures report their cause through the r2/errno ABI (EFAULT,
 *   ENAMETOOLONG, ENOENT, ENOTDIR, EMFILE), and successes leave errno alone.
 *   bmacs relies on ENOENT to tell "new file" apart from other open failures.
 */

#include "../../../root/crt/sys.h"
#include "../../../root/crt/stdio.h"
#include "../../../root/crt/stdlib.h"
#include "../../../root/crt/string.h"
#include "../../../root/crt/errno.h"
#include "../../../root/shell/dirs.h"
#include "../../../root/shell/shell.h"
#include "../../user_test.h"

#define BAD_LOW_USER_POINTER ((char*)0x1000)
#define OVERLONG_COMPONENT_BYTES 256
#define OVERLONG_BUFFER_BYTES 257
#define EXPECTED_TEXT "present"
#define EXPECTED_TEXT_BYTES 7
#define EXPECTED_TEXT_BUFFER_BYTES 8
// docs/syscalls.md: file descriptors occupy slots 0..99.
#define FILE_DESCRIPTOR_SLOTS 100

static int path_is_missing(char* path){ /* Return whether opening the path failed with the missing-file error. */
  int fd = open_existing(path);
  if (fd >= 0){
    close(fd);
    return 0;
  }
  return 1;
}

static int open_existing_errno(char* path){ /* Return errno after a failed open_existing, or -1 if the open unexpectedly succeeded. */
  errno = 0;
  int fd = open_existing(path);
  if (fd >= 0){
    close(fd);
    return -1;
  }
  return errno;
}

static void check_error_reporting(void){ /* Verify open/open_existing failure causes reach errno through r2. */
  char overlong[OVERLONG_BUFFER_BYTES];
  char unterminated[MAX_PATH];

  user_test_expect_eq("open_existing bad pointer reports EFAULT",
    open_existing_errno(BAD_LOW_USER_POINTER), EFAULT);

  memset(overlong, 'n', OVERLONG_COMPONENT_BYTES);
  overlong[OVERLONG_COMPONENT_BYTES] = '\0';
  user_test_expect_eq("open_existing 256-byte component reports ENAMETOOLONG",
    open_existing_errno(overlong), ENAMETOOLONG);

  memset(unterminated, 'u', sizeof(unterminated));
  user_test_expect_eq("open_existing unterminated path reports ENAMETOOLONG",
    open_existing_errno(unterminated), ENAMETOOLONG);

  user_test_expect_eq("open_existing missing file reports ENOENT",
    open_existing_errno("missing-errno"), ENOENT);
  user_test_expect_eq("open_existing missing parent reports ENOENT",
    open_existing_errno("missing-parent/child"), ENOENT);

  // A successful syscall reports r2 == 0, which must not clear a prior cause.
  errno = ENOENT;
  int fd = open_existing("/existing.txt");
  user_test_expect_eq("successful open_existing leaves errno unchanged",
    errno, ENOENT);
  if (fd >= 0){
    close(fd);
  }

  // Creating open() cannot descend through a regular file.
  errno = 0;
  user_test_expect_eq("open through regular file fails",
    open("/existing.txt/child"), -1);
  user_test_expect_eq("open through regular file reports ENOTDIR",
    errno, ENOTDIR);

  // Exhaust the descriptor table, then check that both opens report EMFILE.
  int fds[FILE_DESCRIPTOR_SLOTS];
  int opened = 0;
  while (opened < FILE_DESCRIPTOR_SLOTS){
    fd = open_existing("/");
    if (fd < 0){
      break;
    }
    fds[opened++] = fd;
  }
  user_test_expect_eq("open_existing full table reports EMFILE",
    open_existing_errno("/"), EMFILE);
  errno = 0;
  user_test_expect_eq("open full table fails", open("/existing.txt"), -1);
  user_test_expect_eq("open full table reports EMFILE", errno, EMFILE);
  while (opened > 0){
    close(fds[--opened]);
  }
}

static void run_shell_command(char* command){ /* Feed one command through the bundled shell's normal dispatch buffer. */
  cmd_buf_len = strlen(command);
  memcpy(cmd_buf, command, cmd_buf_len + 1);
  handle_command();
}

int main(void){ /* Verify opening existing files and reporting missing paths. */
  char text[EXPECTED_TEXT_BUFFER_BYTES];
  char overlong[OVERLONG_BUFFER_BYTES];
  char unterminated[MAX_PATH];

  user_test_expect_eq("open_existing rejects low user pointer",
    open_existing(BAD_LOW_USER_POINTER), -1);

  memset(overlong, 'n', OVERLONG_COMPONENT_BYTES);
  overlong[OVERLONG_COMPONENT_BYTES] = '\0';
  user_test_expect_eq("open_existing rejects 256-byte component",
    open_existing(overlong), -1);

  memset(unterminated, 'u', sizeof(unterminated));
  user_test_expect_eq("open_existing rejects unterminated maximum path",
    open_existing(unterminated), -1);

  user_test_expect_eq("missing lookup returns -1",
    open_existing("missing-direct"), -1);
  user_test_expect_eq("missing lookup did not create entry",
    path_is_missing("missing-direct"), 1);

  int fd = open_existing("/existing.txt");
  user_test_expect_eq("open_existing finds existing file", fd >= 0, 1);
  int bytes_read = fd >= 0 ? read(fd, text, EXPECTED_TEXT_BYTES) : -1;
  if (bytes_read >= 0){
    text[bytes_read] = '\0';
  } else {
    text[0] = '\0';
  }
  user_test_expect_eq("open_existing reads existing contents",
    bytes_read == EXPECTED_TEXT_BYTES && streq(text, EXPECTED_TEXT), 1);
  user_test_expect_eq("close existing descriptor",
    fd >= 0 ? close(fd) : -1, 0);

  fd = open_existing("/");
  user_test_expect_eq("open_existing finds existing directory", fd >= 0, 1);
  user_test_expect_eq("close existing directory descriptor",
    fd >= 0 ? close(fd) : -1, 0);

  FILE* input = fopen("missing-fopen", "rb");
  user_test_expect_eq("read-mode fopen rejects missing input", input == NULL, 1);
  user_test_expect_eq("read-mode fopen did not create input",
    path_is_missing("missing-fopen"), 1);

  input = fopen("/existing.txt", "rb");
  user_test_expect_eq("read-mode fopen finds existing input", input != NULL, 1);
  unsigned fread_count = input != NULL
    ? fread(text, 1, EXPECTED_TEXT_BYTES, input) : 0;
  text[fread_count] = '\0';
  user_test_expect_eq("read-mode fopen reads existing contents",
    fread_count == EXPECTED_TEXT_BYTES && streq(text, EXPECTED_TEXT), 1);
  user_test_expect_eq("close read-mode stream",
    input != NULL ? fclose(input) : -1, 0);

  FILE* output = fopen("created-by-fopen", "w");
  user_test_expect_eq("write-mode fopen creates output", output != NULL, 1);
  user_test_expect_eq("close write-mode stream",
    output != NULL ? fclose(output) : -1, 0);
  user_test_expect_eq("created fopen output is visible",
    path_is_missing("created-by-fopen"), 0);

  fd = open("created/by/legacy-open");
  user_test_expect_eq("legacy open retains recursive creation", fd >= 0, 1);
  user_test_expect_eq("close legacy-created descriptor",
    fd >= 0 ? close(fd) : -1, 0);
  user_test_expect_eq("legacy-created file is visible",
    path_is_missing("created/by/legacy-open"), 0);

  struct LinkedDirent* entries = read_directory("missing-directory");
  user_test_expect_eq("directory reader rejects missing path",
    entries == (struct LinkedDirent*)-1, 1);
  user_test_expect_eq("directory reader did not create path",
    path_is_missing("missing-directory"), 1);

  run_shell_command("cat shell-cat-missing");
  user_test_expect_eq("shell cat did not create missing input",
    path_is_missing("shell-cat-missing"), 1);

  run_shell_command("cp shell-cp-missing shell-cp-destination");
  user_test_expect_eq("shell cp did not create missing source",
    path_is_missing("shell-cp-missing"), 1);
  user_test_expect_eq("shell cp did not create destination after source failure",
    path_is_missing("shell-cp-destination"), 1);

  run_shell_command("mv shell-mv-missing shell-mv-destination");
  user_test_expect_eq("shell mv did not create missing source",
    path_is_missing("shell-mv-missing"), 1);
  user_test_expect_eq("shell mv did not create destination after source failure",
    path_is_missing("shell-mv-destination"), 1);

  check_error_reporting();

  printf("***Done.\n", NULL);
  return 0;
}
