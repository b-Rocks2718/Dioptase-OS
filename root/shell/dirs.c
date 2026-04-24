#include "dirs.h"
#include "../crt/string.h"
#include "../crt/sys.h"
#include "../crt/print.h"
#include "../crt/vga.h"


void *malloc(unsigned size);
void free(void* p);

#define ENTRIES_PER_LINE 4
#define SPACES_PER_TAB 8

#define PRINT_BUFFER_SIZE 256

struct LinkedDirent* create_linked_dirent(struct linux_dirent* dirent) {
  struct LinkedDirent* entry = malloc(sizeof(struct LinkedDirent) + dirent->d_reclen - sizeof(struct linux_dirent));
  memcpy(&entry->dirent, dirent, dirent->d_reclen);
  entry->d_type = *((char*)dirent + dirent->d_reclen - 1);
  entry->next = 0;
  return entry;
}

void destroy_linked_dirents(struct LinkedDirent* head) {
  struct LinkedDirent* current = head;
  while (current != 0){
    struct LinkedDirent* next = current->next;
    free(current);
    current = next;
  }
}

struct LinkedDirent* read_directory(char* path) {
  int fd = open(path);
  if (fd < 0) {
    return (struct LinkedDirent*) -1;
  }

  char* buffer = malloc(1024);
  struct LinkedDirent* head = 0;
  struct LinkedDirent* tail = 0;
  while (1) {
    int n = getdents(fd, buffer, 1024);
    if (n < 0) {
      // Error.
      close(fd);
      destroy_linked_dirents(head);
      free(buffer);
      return (struct LinkedDirent*) -1;
    }
    if (n == 0) {
      // No more entries.
      break;
    }

    int offset = 0;
    while (offset < n) {
      struct linux_dirent* dirent = (struct linux_dirent*) (buffer + offset);
      struct LinkedDirent* new_entry = create_linked_dirent(dirent);
      if (head == 0) {
        head = new_entry;
        tail = new_entry;
      } else {
        tail->next = new_entry;
        tail = new_entry;
      }
      offset += dirent->d_reclen;
    }
  }
  close(fd);
  free(buffer);
  return head;
}

// Returns new index. If keep_together_length > 1, ensures first characters are printed together.
unsigned add_to_print_buffer(char* print_buffer, unsigned index, char* to_add, unsigned keep_together_length) {
  // Print earlier if needed.
  if (keep_together_length > 1 && index + keep_together_length >= PRINT_BUFFER_SIZE) {
    print_buffer[index] = 0;
    puts(print_buffer);
    index = 0;
  }
  while (*to_add != 0) {
    if (index >= PRINT_BUFFER_SIZE - 1) {
      // Buffer full. Print and reset.
      print_buffer[index] = 0;
      puts(print_buffer);
      index = 0;
    }
    // Add character.
    print_buffer[index++] = *to_add;
    to_add++;
  }
  return index;
}

void print_print_buffer(char* print_buffer, unsigned index) {
  print_buffer[index] = 0;
  puts(print_buffer);
}

void print_directory(struct LinkedDirent* head, bool skip_current_and_parent) {
  if (head == 0) {
    return;
  }

  int count = 0;

  unsigned longest[ENTRIES_PER_LINE] = {0};
  // Find longest names for formatting.
  for (struct LinkedDirent* current = head; current != 0; current = current->next) {
    char* name = &current->dirent.d_name;

    // Skip "." and ".." if not including.
    if (skip_current_and_parent && name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
      continue;
    }
    // Skip "lost+found".
    if (strcmp(name, "lost+found") == 0) {
      continue;
    }

    unsigned length = strlen(name);
    if (length > longest[count % ENTRIES_PER_LINE]) {
      longest[count % ENTRIES_PER_LINE] = length;
    }
    count++;
  }

  for (int i = 0; i < ENTRIES_PER_LINE; i++) {
    // Add one for a space, and then round up.
    longest[i] = (longest[i] + SPACES_PER_TAB) & ~(SPACES_PER_TAB - 1);
  }

  count = 0;
  char print_buffer[PRINT_BUFFER_SIZE];
  unsigned buffer_index = 0;
  for (struct LinkedDirent* current = head; current != 0; current = current->next) {
    char* name = &current->dirent.d_name;

    // Skip "." and ".." if not including.
    if (skip_current_and_parent && name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
      continue;
    }
    // Skip "lost+found".
    if (strcmp(name, "lost+found") == 0) {
      continue;
    }

    // Colors I stole from bash.
    char* color;
    switch (current->d_type) {
      case DT_FIFO:
        color = "\x1b[49m";
        break;
      case DT_CHR:
        color = "\x1b[48m";
        break;
      case DT_DIR:
        color = "\x1b[34m";
        break;
      case DT_BLK:
        color = "\x1b[48m";
        break;
      case DT_REG:
        color = "\x1b[37m";
        break;
      case DT_LNK:
        color = "\x1b[36m";
        break;
      case DT_SOCK:
        color = "\x1b[35m";
        break;
      case DT_WHT:
        color = "\x1b[37m";
        break;
      default: // DT_UNKNOWN or other type.
        color = "\x1b[31m"; // Bright red.
        break;
    }
    buffer_index = add_to_print_buffer(print_buffer, buffer_index, color, 5);
    buffer_index = add_to_print_buffer(print_buffer, buffer_index, name, 0);
    // Reset to white.
    buffer_index = add_to_print_buffer(print_buffer, buffer_index, "\x1b[37m", 5);

    unsigned name_length = strlen(name);
    unsigned padding = longest[count % ENTRIES_PER_LINE] - name_length;
    for (unsigned i = 0; i < padding; i++) {
      buffer_index = add_to_print_buffer(print_buffer, buffer_index, " ", 0);
    }
    count++;
    if (count % ENTRIES_PER_LINE == 0) {
      // Print at end of line.
      buffer_index = add_to_print_buffer(print_buffer, buffer_index, "\n", 0);
      print_print_buffer(print_buffer, buffer_index);
      buffer_index = 0;
    }
  }
  // Reset color.
  buffer_index = add_to_print_buffer(print_buffer, buffer_index, "\x1b[37m", 5);

  // Final newline if we didn't end on one.
  if (count % ENTRIES_PER_LINE != 0) {
    buffer_index = add_to_print_buffer(print_buffer, buffer_index, "\n", 0);
  }

  // Print rest of buffer.
  print_print_buffer(print_buffer, buffer_index);
}

struct LinkedDirent* tab_complete_directory(char* prefix) {
  // Find last '/' in prefix.
  int last_slash = -1;
  for (int i = 0; prefix[i] != 0; i++) {
    if (prefix[i] == '/') {
      last_slash = i;
    }
  }
  char* prefix_base = malloc(last_slash + 2); // For '/' and 0.

  struct LinkedDirent* head;
  if (last_slash == -1) {
    // No base path.
    prefix_base[0] = 0;
    head = read_directory(".");
  } else {
    memcpy(prefix_base, prefix, last_slash + 1);
    prefix_base[last_slash + 1] = 0;
    head = read_directory(prefix_base);
  }

  if (head == 0) {
    // No matches.
    free(prefix_base);
    return 0;
  }

  // Match rest of prefix.
  char* prefix_rest = prefix + last_slash + 1;
  unsigned prefix_rest_length = strlen(prefix_rest);
  struct LinkedDirent* matches_head = 0;
  struct LinkedDirent* matches_tail = 0;
  for (struct LinkedDirent* current = head; current != 0;) {
    bool match = 1;
    char* name = &current->dirent.d_name;
    for (unsigned i = 0; i < prefix_rest_length; i++) {
      if (name[i] != prefix_rest[i] || name[i] == 0) {
        match = 0;
        break;
      }
    }
    struct LinkedDirent* next = current->next;
    current->next = 0;
    if (match) {
      if (matches_head == 0) {
        matches_head = current;
        matches_tail = current;
      } else {
        matches_tail->next = current;
        matches_tail = current;
      }
    } else {
      // No match. Free.
      free(current);
    }
    current = next;
  }
  free(prefix_base);
  return matches_head;
}
