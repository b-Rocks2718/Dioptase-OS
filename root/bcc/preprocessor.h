#ifndef PREPROCESSOR_H
#define PREPROCESSOR_H

#include "../crt/stdbool.h"
#include "../crt/stddef.h"

#include "source_location.h"

// The root translation unit is not counted. At most this many recursively
// included files may be active at once. This implementation-defined bound
// keeps malformed or adversarial include graphs within the fixed user stack.
#define PREPROCESSOR_MAX_INCLUDE_DEPTH 32

// Own interned file name storage for source mappings.
// Names entries are heap-allocated NUL-terminated strings.
struct FileTable {
  char** names;
  size_t count;
  size_t cap;
};

// Bundle preprocessed output with source mapping metadata.
struct PreprocessResult {
  char* text;
  struct SourceMapping map;
  struct FileTable file_table;
};

// Preprocess a source buffer (comments, directives, object-like macros).
// Returns true on success and fills result; false on error.
// Supports quoted relative includes and angle-bracket
//                         includes rooted at /crt; macros are object-like only.
//                         Active include cycles and nesting beyond
//                         PREPROCESSOR_MAX_INCLUDE_DEPTH are rejected.
bool preprocess(char * prog, char* filename, int num_defines,
                char* * defines, struct PreprocessResult* result);

// Free all storage owned by a PreprocessResult.
// Safe to call with partially initialized results.
void destroy_preprocess_result(struct PreprocessResult* result);

#endif
