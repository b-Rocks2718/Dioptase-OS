/*
 * Tests reusable shell and directory behavior:
 * - tab_complete_directory and print_directory layout
 * - a nonempty directory whose entries are all filtered from display
 * - real `mv` command dispatch rejecting a final-component destination
 *   symlink before it can unlink the source
 */

#include "../../../root/crt/sys.h"
#include "../../../root/crt/stdlib.h"
#include "../../../root/crt/string.h"
#include "../../../root/crt/print.h"
#include "../../../root/shell/dirs.h"
#include "../../../root/shell/shell.h"
#include "../../user_test.h"

#define MV_TEST_DIRECTORY "folder/longer_name_folder0"
#define MV_SOURCE_PATH "inner_file0"
#define MV_DESTINATION_LINK "../file_link"
#define MV_TEST_CONTENT "source-preserved"
#define MV_TEST_CONTENT_BYTES 16
#define MV_TEST_BUFFER_BYTES 17
#define MV_TEST_COMMAND "mv inner_file0 ../file_link"

void test_directory(char* path) { /* Test directory. */
  struct LinkedDirent* head = tab_complete_directory(path, false);
  puts("***");
  print_directory(head, true);
  puts("\n");
  destroy_linked_dirents(head);
}

struct LinkedDirent* create_linkeddirent(char d_type, char* name, struct LinkedDirent* next) { /* Create linkeddirent. */
  unsigned name_length = strlen(name);
  struct LinkedDirent* entry = malloc(sizeof(struct LinkedDirent) + name_length + 1);
  entry->d_type = d_type;
  memcpy(&entry->dirent.d_name, name, name_length + 1);
  entry->next = next;
  return entry;
}

int main(void) { /* Verify shell directory tab completion and entry filtering. */
  test_directory("");
  test_directory("folder");
  test_directory("folder/"); // Longer name should spill onto next line and thus not be printed.
  test_directory("folder/ljksdf"); // No matches.
  test_directory("/folder/inner_fi");
  test_directory("notreal/");
  test_directory("notreal/wow");

  // Create fake entries for all file types.
  puts("\n");
  struct LinkedDirent* head = create_linkeddirent(DT_BLK, "block_device", 0);
  head = create_linkeddirent(DT_DIR, "directory", head);
  head = create_linkeddirent(DT_CHR, "character_device", head);
  head = create_linkeddirent(DT_FIFO, "fifo", head);
  head = create_linkeddirent(DT_UNKNOWN, "unknown", head);
  puts("***");
  print_directory(head, false);
  puts("\n");
  destroy_linked_dirents(head);

  head = create_linkeddirent(DT_WHT, "whiteout", 0);
  head = create_linkeddirent(DT_SOCK, "socket", head);
  head = create_linkeddirent(DT_LNK, "symbolic_link", head);
  head = create_linkeddirent(DT_REG, "regular_file", head);
  puts("***");
  print_directory(head, false);
  puts("\n");
  destroy_linked_dirents(head);

  // Exercise the early return for a real nonempty list with no visible names.
  // The leading marker and trailing report must remain one line in the golden
  // output; any formatting emitted by print_directory appears between them and
  // makes the regression fail.
  head = create_linkeddirent(DT_DIR, "lost+found", 0);
  head = create_linkeddirent(DT_DIR, "..", head);
  head = create_linkeddirent(DT_DIR, ".", head);
  puts("***");
  print_directory(head, true);
  puts("PASS all-filtered directory emits no formatting\n");
  destroy_linked_dirents(head);

  /*
   * Enter the source directory so MV_SOURCE_PATH is a basename accepted by
   * this kernel's unlink syscall. The checked-in destination one directory up
   * is a relative symlink back to that source. Without the guard, copy succeeds
   * through the alias and mv really does unlink MV_SOURCE_PATH, leaving the
   * destination dangling. Seed nonempty data so reopening the source (open
   * creates missing files in this ABI) still detects that loss.
   */
  int source_ready = chdir(MV_TEST_DIRECTORY) == 0;
  int source_fd = source_ready ? open(MV_SOURCE_PATH) : -1;
  unsigned test_content_bytes = MV_TEST_CONTENT_BYTES;
  source_ready = source_ready && source_fd >= 0;
  if (source_ready){
    int write_result = write(source_fd, MV_TEST_CONTENT, test_content_bytes);
    int truncate_result = truncate(source_fd, test_content_bytes);
    int close_result = close(source_fd);
    source_ready = write_result == (int)test_content_bytes &&
                   truncate_result == 0 && close_result == 0;
  }
  user_test_expect_eq("prepare mv symlink-alias source", source_ready, 1);

  cmd_buf_len = strlen(MV_TEST_COMMAND);
  memcpy(cmd_buf, MV_TEST_COMMAND, cmd_buf_len);
  cmd_buf[cmd_buf_len] = '\0';
  handle_command();

  char observed[MV_TEST_BUFFER_BYTES];
  source_fd = open(MV_SOURCE_PATH);
  int observed_bytes = source_fd >= 0
                         ? read(source_fd, observed, test_content_bytes)
                         : -1;
  int source_close_result = source_fd >= 0 ? close(source_fd) : -1;
  char destination_probe;
  int destination_is_still_symlink =
    readlink(MV_DESTINATION_LINK, &destination_probe,
             sizeof(destination_probe)) >= 0;
  observed[test_content_bytes] = '\0';
  int source_preserved = observed_bytes == (int)test_content_bytes &&
                         source_close_result == 0 &&
                         streq(observed, MV_TEST_CONTENT) &&
                         destination_is_still_symlink;
  user_test_expect_eq("mv rejects direct symlink destination and preserves source/link",
                      source_preserved, 1);

  printf("***Done.\n", NULL);

  return 0;
}
