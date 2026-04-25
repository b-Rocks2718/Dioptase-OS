#include "source_location.h"

#include "../crt/string.h"

static char* source_text_ptr = NULL;
static char* source_file_ptr = NULL;
static struct SourceMapping* source_map_ptr = NULL;

// Purpose: Measure one byte offset inside the current source buffer.
// Inputs: start/end point into the same preprocessed source text.
// Outputs: Returns the byte offset from start to end.
// Invariants/Assumptions: docs/abi.md defines pointers as 4-byte values, so
// the bootstrap compiler can recover byte distances through unsigned
// arithmetic even though it rejects direct pointer subtraction.
static size_t source_span_len(char* start, char* end) {
  return (size_t)((unsigned)end - (unsigned)start);
}

void set_source_context(char* filename, char* text) {
  set_source_context_with_map(filename, text, NULL);
}

void set_source_context_with_map(char* filename, char* text, struct SourceMapping* map) {
  source_file_ptr = filename;
  source_text_ptr = text;
  source_map_ptr = map;
}

char* source_filename(void) {
  return source_file_ptr ? source_file_ptr : "<input>";
}

char* source_filename_for_ptr(char* ptr) {
  if (source_map_ptr == NULL || source_text_ptr == NULL || ptr == NULL) {
    return source_filename();
  }

  char* end = source_text_end();
  if (end != NULL && ptr > end) ptr = end;
  if (ptr < source_text_ptr) return source_filename();

  size_t offset = source_span_len(source_text_ptr, ptr);
  if (offset >= source_map_ptr->length) {
    if (source_map_ptr->length == 0) return source_filename();
    offset = source_map_ptr->length - 1;
  }
  char* mapped = source_map_ptr->entries[offset].filename;
  return mapped != NULL ? mapped : source_filename();
}

char* source_text(void) {
  return source_text_ptr;
}

char* source_text_end(void) {
  if (source_text_ptr == NULL) return NULL;
  return source_text_ptr + strlen(source_text_ptr);
}

struct SourceLocation source_location_from_ptr(char* ptr) {
  struct SourceLocation loc = {0, 0, 0};
  if (source_text_ptr == NULL || ptr == NULL) return loc;

  char* end = source_text_end();
  if (end != NULL && ptr > end) ptr = end;
  if (ptr < source_text_ptr) return loc;

  if (source_map_ptr != NULL && source_map_ptr->entries != NULL && source_map_ptr->length > 0) {
    size_t offset = source_span_len(source_text_ptr, ptr);
    if (offset >= source_map_ptr->length) {
      offset = source_map_ptr->length - 1;
    }
    loc.line = source_map_ptr->entries[offset].line;
    loc.column = source_map_ptr->entries[offset].column;
    loc.offset = offset;
    return loc;
  }

  size_t line = 1;
  size_t column = 1;
  char* cur = source_text_ptr;

  // Walk the buffer to compute a 1-based line/column for ptr.
  while (cur < ptr && *cur != '\0') {
    if (*cur == '\n') {
      line++;
      column = 1;
    } else {
      column++;
    }
    cur++;
  }

  loc.line = line;
  loc.column = column;
  loc.offset = source_span_len(source_text_ptr, ptr);
  return loc;
}
