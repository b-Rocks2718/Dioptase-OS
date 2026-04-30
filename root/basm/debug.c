#include "debug.h"

#include "../crt/print.h"
#include "../crt/stdlib.h"
#include "../crt/string.h"

// Debug metadata is written after the preprocessed source buffers are freed, so
// it cannot retain borrowed Slice views into those transient buffers.
static struct Slice* duplicate_slice(struct Slice* slice){
  struct Slice* copy = malloc(sizeof(struct Slice));
  char* text = malloc(slice->len);
  memcpy(text, slice->start, slice->len);
  copy->start = text;
  copy->len = slice->len;
  return copy;
}

static void destroy_owned_slice(struct Slice* slice){
  if (slice == NULL) return;
  free(slice->start);
  free(slice);
}

struct DebugInfoList* create_debug_info_list(void){
  struct DebugInfoList* list = malloc(sizeof(struct DebugInfoList));
  list->head = NULL;
  list->tail = NULL;
  return list;
}

void add_debug_local(struct DebugInfoList* debug_list, struct Slice* name, int offset, unsigned size, unsigned addr){
  // create new DebugLocal
  struct DebugLocal* local = malloc(sizeof(struct DebugLocal));
  local->name = duplicate_slice(name);
  local->offset = offset;
  local->size = size;
  local->addr = addr;
  // create new DebugEntry
  struct DebugEntry* entry = malloc(sizeof(struct DebugEntry));
  entry->type = DEBUG_INFO_LOCALS;
  entry->info.locals = local;
  entry->next = NULL;
  // append to debug_list
  if (debug_list->head == NULL){
    debug_list->head = entry;
    debug_list->tail = entry;
  } else {
    debug_list->tail->next = entry;
    debug_list->tail = entry;
  }
}

void add_debug_line(struct DebugInfoList* debug_list, struct Slice* file_name, int line_number, unsigned addr){
  // create new DebugLine
  struct DebugLine* line = malloc(sizeof(struct DebugLine));
  line->file_name = duplicate_slice(file_name);
  line->line_number = line_number;
  line->addr = addr;
  // create new DebugEntry
  struct DebugEntry* entry = malloc(sizeof(struct DebugEntry));
  entry->type = DEBUG_INFO_LINES;
  entry->info.lines = line;
  entry->next = NULL;
  // append to debug_list
  if (debug_list->head == NULL){
    debug_list->head = entry;
    debug_list->tail = entry;
  } else {
    debug_list->tail->next = entry;
    debug_list->tail = entry;
  }
}

void fprint_debug_info_list(int file, struct DebugInfoList* debug_list){
  struct DebugEntry* current = debug_list->head;
  while (current != NULL){
    if (current->type == DEBUG_INFO_LOCALS){
      struct DebugLocal* local = current->info.locals;
      int args[5];
      args[0] = (int)local->name->len;
      args[1] = (int)local->name->start;
      args[2] = local->offset;
      args[3] = local->size;
      args[4] = local->addr;
      fdprintf(file, "#local %.*s %d %u %08X\n", args);
    } else if (current->type == DEBUG_INFO_LINES){
      struct DebugLine* line = current->info.lines;
      int args[4];
      args[0] = (int)line->file_name->len;
      args[1] = (int)line->file_name->start;
      args[2] = line->line_number;
      args[3] = line->addr;
      fdprintf(file, "#line %.*s %d %08X\n", args);
    }
    current = current->next;
  }
}

void destroy_debug_info_list(struct DebugInfoList* debug_list){
  struct DebugEntry* current = debug_list->head;
  while (current != NULL){
    struct DebugEntry* next = current->next;
    // free the contained info based on type
    if (current->type == DEBUG_INFO_LOCALS){
      struct DebugLocal* local = current->info.locals;
      destroy_owned_slice(local->name);
      free(local);
    } else if (current->type == DEBUG_INFO_LINES){
      struct DebugLine* line = current->info.lines;
      destroy_owned_slice(line->file_name);
      free(line);
    }
    free(current);
    current = next;
  }
  free(debug_list);
}
