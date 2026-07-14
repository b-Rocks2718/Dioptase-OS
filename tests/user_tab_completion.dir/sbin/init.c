/*
 * Tests the tab completion code in the shell.
 * - tab_complete_directory.
 * - print_directory.
 */

#include "../../../root/crt/sys.h"
#include "../../../root/crt/stdlib.h"
#include "../../../root/crt/string.h"
#include "../../../root/crt/print.h"
#include "../../../root/shell/dirs.h"

void test_directory(char* path) {
  struct LinkedDirent* head = tab_complete_directory(path, false);
  puts("***");
  print_directory(head, true);
  puts("\n");
  destroy_linked_dirents(head);
}

struct LinkedDirent* create_linkeddirent(char d_type, char* name, struct LinkedDirent* next) {
  unsigned name_length = strlen(name);
  struct LinkedDirent* entry = malloc(sizeof(struct LinkedDirent) + name_length + 1);
  entry->d_type = d_type;
  memcpy(&entry->dirent.d_name, name, name_length + 1);
  entry->next = next;
  return entry;
}

int main(void) {
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

  printf("***Done.\n", NULL);

  return 0;
}
