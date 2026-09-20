/*
 * Exercises the lookup-only open contract through both the raw CRT wrapper and
 * real bundled read paths:
 * - existing files and directories still receive ordinary descriptors
 * - invalid, unterminated, and overlong user paths fail without a kernel panic
 * - repeated missing lookups and read-mode fopen do not create entries
 * - write-mode fopen and legacy open retain intentional creation
 * - shell cat/cp/mv missing sources do not synthesize source or destination
 */

#include "../../../root/crt/sys.h"
#include "../../../root/crt/stdio.h"
#include "../../../root/crt/stdlib.h"
#include "../../../root/crt/string.h"
#include "../../../root/shell/dirs.h"
#include "../../../root/shell/shell.h"
#include "../../user_test.h"

#define BAD_LOW_USER_POINTER ((char*)0x1000)
#define OVERLONG_COMPONENT_BYTES 256
#define OVERLONG_BUFFER_BYTES 257
#define EXPECTED_TEXT "present"
#define EXPECTED_TEXT_BYTES 7
#define EXPECTED_TEXT_BUFFER_BYTES 8

static int path_is_missing(char* path){ /* Return whether opening the path failed with the missing-file error. */
  int fd = open_existing(path);
  if (fd >= 0){
    close(fd);
    return 0;
  }
  return 1;
}

static void run_shell_command(char* command){ /* Run shell command. */
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

  printf("***Done.\n", NULL);
  return 0;
}
