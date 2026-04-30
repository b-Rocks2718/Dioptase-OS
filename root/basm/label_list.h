#ifndef LABEL_LIST_H
#define LABEL_LIST_H

#include "../crt/stdbool.h"

struct LabelEntry {
  char* name;
  bool is_data;
  unsigned addr;
};

struct LabelList {
  struct LabelEntry* entries;
  unsigned size;
  unsigned capacity;
};

struct LabelList* create_label_list(unsigned capacity);

void label_list_append(struct LabelList* list, char* name, unsigned len, unsigned addr, bool is_data);

void destroy_label_list(struct LabelList* list);

void fprint_label_list(int file, struct LabelList* list);
void fprint_label_list_kernel(int file, struct LabelList* list);

#endif  // LABEL_LIST_H
