#include "../crt/print.h"
#include "../crt/stdlib.h"
#include "../crt/string.h"
#include "label_list.h"

// Allocate an empty label list with the requested initial capacity.
struct LabelList* create_label_list(unsigned capacity){
  struct LabelList* list = malloc(sizeof(struct LabelList));
  if (capacity == 0) capacity = 16;
  list->entries = malloc(sizeof(struct LabelEntry) * capacity);
  list->size = 0;
  list->capacity = capacity;
  return list;
}

// Return whether a stored label has the same name, address, and data flag.
static bool label_entry_matches(struct LabelEntry* entry, char* name, unsigned len, unsigned addr, bool is_data){
  if (entry->addr != addr) return false;
  if (entry->is_data != is_data) return false;
  if (strlen(entry->name) != len) return false;
  return strncmp(entry->name, name, len) == 0;
}

// Double the label-entry array while preserving owned name pointers.
static struct LabelEntry* grow_label_entries(struct LabelEntry* old_entries, unsigned old_capacity){
  unsigned new_capacity = old_capacity * 2;
  struct LabelEntry* new_entries = malloc(sizeof(struct LabelEntry) * new_capacity);
  memcpy(new_entries, old_entries, sizeof(struct LabelEntry) * old_capacity);
  free(old_entries);
  return new_entries;
}

// Add a label unless an identical name/address/kind entry is already present.
void label_list_append(struct LabelList* list, char* name, unsigned len, unsigned addr, bool is_data){
  for (unsigned i = 0; i < list->size; ++i){
    if (label_entry_matches(&list->entries[i], name, len, addr, is_data)) return;
  }

  if (list->size == list->capacity){
    list->capacity *= 2;
    list->entries = grow_label_entries(list->entries, list->size);
  }

  char* name_copy = malloc(len + 1);
  memcpy(name_copy, name, len);
  name_copy[len] = '\0';

  list->entries[list->size].name = name_copy;
  list->entries[list->size].addr = addr;
  list->entries[list->size].is_data = is_data;
  list->size++;
}

// Free every owned label name and all list storage.
void destroy_label_list(struct LabelList* list){
  if (list == NULL) return;
  for (unsigned i = 0; i < list->size; ++i){
    free(list->entries[i].name);
  }
  free(list->entries);
  free(list);
}

// Write labels in the debugger's tagged text format.
void fprint_label_list(int file, struct LabelList* list){
  if (list == NULL) return;
  for (unsigned i = 0; i < list->size; ++i){
    int args[3];
    args[0] = (int)(list->entries[i].is_data ? "data" : "label");
    args[1] = (int)list->entries[i].name;
    args[2] = list->entries[i].addr;
    fdprintf(file, "#%s %s %08X\n", args);
  }
}

// Emit label metadata for kernel outputs (no data/text distinction).
// Writes "#label <name> <addr>" lines, ignoring is_data.
void fprint_label_list_kernel(int file, struct LabelList* list){
  if (list == NULL) return;
  for (unsigned i = 0; i < list->size; ++i){
    int args[2];
    args[0] = (int)list->entries[i].name;
    args[1] = list->entries[i].addr;
    fdprintf(file, "#label %s %08X\n", args);
  }
}
