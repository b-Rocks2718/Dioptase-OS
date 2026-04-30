#ifndef SOURCE_LOCATION_H
#define SOURCE_LOCATION_H

#include "../crt/stddef.h"

// Purpose: Represent a single source coordinate in the original input.
// Inputs/Outputs: Stored alongside preprocessed output for error reporting.
// Invariants/Assumptions: line/column are 1-based; filename points to stable storage.
struct SourceMappingEntry {
  char* filename;
  size_t line;
  size_t column;
};

// Purpose: Map preprocessed output offsets back to original source coordinates.
// Inputs/Outputs: entries has one element per output byte (excluding NUL).
// Invariants/Assumptions: length matches the preprocessed buffer length.
struct SourceMapping {
  struct SourceMappingEntry* entries;
  size_t length;
};

struct SourceLocation {
  size_t line;
  size_t column;
  size_t offset;
};

void set_source_context(char* filename, char* text);

void set_source_context_with_map(char* filename, char* text, struct SourceMapping* map);

char* source_filename(void);

char* source_filename_for_ptr(char* ptr);

char* source_text(void);

char* source_text_end(void);

struct SourceLocation source_location_from_ptr(char* ptr);

#endif // SOURCE_LOCATION_H
