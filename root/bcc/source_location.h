#ifndef SOURCE_LOCATION_H
#define SOURCE_LOCATION_H

#include "../crt/stddef.h"

// Map one preprocessed byte position to its original file coordinate.
// Stored alongside preprocessed output for error reporting.
// Line/column are 1-based; filename points to stable storage.
struct SourceMappingEntry {
  char* filename;
  size_t line;
  size_t column;
};

// Map preprocessed output offsets back to original source coordinates.
// Length matches the preprocessed buffer length.
struct SourceMapping {
  struct SourceMappingEntry* entries;
  size_t length;
};

// Store a source line, column, and byte offset for diagnostics.
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
