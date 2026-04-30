#ifndef DIRS_H
#define DIRS_H

#include "../crt/dirent.h"
#include "../crt/stdbool.h"

// Linked-list representation.
struct LinkedDirent {
  struct LinkedDirent* next;
  char d_type;
  struct linux_dirent dirent;
};

struct LinkedDirent* create_linked_dirent(struct linux_dirent* dirent);
void destroy_linked_dirents(struct LinkedDirent* head);

struct LinkedDirent* read_directory(char* path);
void print_directory(struct LinkedDirent* head, bool skip_current_and_parent); 

struct LinkedDirent* tab_complete_directory(char* prefix, bool include_commands);

#endif // DIRS_H

