#include "../crt/stdlib.h"
#include "../crt/stdio.h"
#include "../crt/print.h"
#include "../crt/stdbool.h"
#include "../crt/string.h"
#include "../crt/ctype.h"
#include "../crt/limits.h"

#include "preprocessor.h"

// Purpose: Implement a minimal preprocessor with comment stripping, object-like
//          macro expansion, and basic conditional/include handling.
// Inputs: NUL-terminated source buffers and CLI define strings.
// Outputs: Newly allocated, NUL-terminated preprocessed source on success.
// Invariants/Assumptions: Supports #include "path" and #include <path>, plus
//                         #define, #ifdef/#ifndef/#else/#endif; no
//                         function-like macros or #undef.

// Purpose: Track a growable output buffer and source mapping during preprocessing.
// Inputs/Outputs: Mutated in-place by buffer helpers.
// Invariants/Assumptions: cap >= len; data/map are heap-allocated or NULL.
struct Buffer {
  char* data;
  struct SourceMappingEntry* map;
  size_t len;
  size_t cap;
};

// Purpose: Represent object-like macros as a linked list.
// Inputs/Outputs: Owned by the preprocessor and freed at teardown.
// Invariants/Assumptions: name/value are heap strings; no function-like macros.
struct Macro {
  char* name;
  size_t name_len;
  char* value;
  size_t value_len;
  struct Macro* next;
};

// Purpose: Identify builtin macro names with special expansion behavior.
// Inputs/Outputs: Read-only names, no state changes.
// Invariants/Assumptions: Names are ASCII and compared case-sensitively.
static char kBuiltinFileMacro[9] = "__FILE__";
static char kBuiltinLineMacro[9] = "__LINE__";
#define K_BUILTIN_FILE_MACRO_LEN 8
#define K_BUILTIN_LINE_MACRO_LEN 8
// Decimal digits for a 32-bit size_t line number plus a trailing NUL.
#define LINE_NUMBER_BUFFER_CAP 11
#define FILE_TABLE_INITIAL_CAP 8
#define SYSTEM_INCLUDE_ROOT "/crt"

// Purpose: Store preprocessed output and source mapping together.
// Inputs/Outputs: Filled by preprocess_buffer and consumed by callers.
// Invariants/Assumptions: map.length matches strlen(text).
struct PreprocessOutput {
  char* text;
  struct SourceMapping map;
};

// Purpose: Forward declarations for file table helpers used before definition.
// Inputs/Outputs: See file_table_* implementations below.
// Invariants/Assumptions: Filename storage is owned by the file table.
static void file_table_init(struct FileTable* table);
static char* file_table_intern(struct FileTable* table, char* name);
static void file_table_destroy(struct FileTable* table);

// Purpose: Track one level of #ifdef/#ifndef state.
// Inputs/Outputs: Stored in the conditional stack; updated by directives.
// Invariants/Assumptions: parent_active reflects the outer active state.
struct IfState {
  bool parent_active;
  bool condition_true;
  bool in_else;
};

// Purpose: Maintain a stack of nested #if states with current activity.
// Inputs/Outputs: Mutated by ifstack_* helpers.
// Invariants/Assumptions: current_active mirrors the top-of-stack evaluation.
struct IfStack {
  struct IfState* items;
  size_t count;
  size_t cap;
  bool current_active;
};

// Purpose: Report a fixed preprocessor error with filename and line context.
// Inputs: filename and line_no describe the error location; message is fixed text.
// Outputs: Writes a formatted message to stderr.
// Invariants/Assumptions: filename is non-NULL when reporting a file-backed input.
static void preprocessor_error0(char* filename, size_t line_no, char* message) {
  int args[3];

  args[0] = (int)filename;
  args[1] = (int)line_no;
  args[2] = (int)message;
  fdprintf(STDERR, "Preprocessor error at %s:%zu: %s\n", args);
}

static void preprocessor_error1_str(char* filename, size_t line_no,
                                    char* fmt, char* arg0) {
  int args[3];

  args[0] = (int)filename;
  args[1] = (int)line_no;
  args[2] = (int)arg0;
  fdprintf(STDERR, fmt, args);
}

static void preprocessor_error1_slice(char* filename, size_t line_no,
                                      char* fmt, size_t arg0_len, char* arg0_text) {
  int args[4];

  args[0] = (int)filename;
  args[1] = (int)line_no;
  args[2] = (int)arg0_len;
  args[3] = (int)arg0_text;
  fdprintf(STDERR, fmt, args);
}

// Purpose: Measure one byte span inside a source buffer.
// Inputs: start/end point into the same underlying character array.
// Outputs: Returns the number of bytes between start and end.
// Invariants/Assumptions: docs/abi.md defines pointers as 4-byte values, so
// the bootstrap compiler can materialize char-pointer differences through
// unsigned arithmetic even though it rejects direct pointer subtraction.
static size_t span_len(char* start, char* end) {
  return (size_t)((unsigned)end - (unsigned)start);
}

// Purpose: Initialize a buffer with the requested capacity.
// Inputs: buf is the buffer to initialize; cap is the initial capacity.
// Outputs: Returns true on success and zeroes buf length.
// Invariants/Assumptions: buf is non-NULL; caller frees buf->data and buf->map.
static bool buffer_init(struct Buffer* buf, size_t cap) {
  buf->data = malloc(cap);
  if (buf->data == NULL) return false;
  buf->map = malloc(cap * sizeof(*buf->map));
  if (buf->map == NULL) {
    free(buf->data);
    buf->data = NULL;
    return false;
  }
  buf->len = 0;
  buf->cap = cap;
  return true;
}

// Purpose: Ensure the buffer can append add bytes plus a trailing NUL.
// Inputs: buf is the target buffer; add is additional bytes to reserve.
// Outputs: Returns true on success and grows the buffer if needed.
// Invariants/Assumptions: buffer_init was called and buf->data/map are heap-allocated.
static bool buffer_reserve(struct Buffer* buf, size_t add) {
  if (buf->len + add + 1 <= buf->cap) return true;
  size_t new_cap = buf->cap == 0 ? 64 : buf->cap;
  while (new_cap < buf->len + add + 1) new_cap *= 2;
  char* next = realloc(buf->data, new_cap);
  if (next == NULL) return false;
  buf->data = next;
  struct SourceMappingEntry* next_map = realloc(buf->map, new_cap * sizeof(*buf->map));
  if (next_map == NULL) return false;
  buf->map = next_map;
  buf->cap = new_cap;
  return true;
}

// Purpose: Append a single character and mapping entry to the buffer.
// Inputs: buf is the target buffer; c is the character; loc is the source mapping.
// Outputs: Returns true on success and increments buf->len.
// Invariants/Assumptions: buffer_reserve must succeed for append.
static bool buffer_append_char(struct Buffer* buf, char c, struct SourceMappingEntry loc) {
  if (!buffer_reserve(buf, 1)) return false;
  buf->data[buf->len++] = c;
  buf->map[buf->len - 1] = loc;
  return true;
}

// Purpose: Append a string slice with a single source location.
// Inputs: buf is the target buffer; s/len describe bytes; loc is the source mapping.
// Outputs: Returns true on success and increments buf->len.
// Invariants/Assumptions: s may be non-NUL-terminated; loc applies to all bytes.
static bool buffer_append_str_with_loc(struct Buffer* buf, char* s, size_t len,
                                       struct SourceMappingEntry loc) {
  if (!buffer_reserve(buf, len)) return false;
  memcpy(buf->data + buf->len, s, len);
  for (size_t i = 0; i < len; ++i) {
    buf->map[buf->len + i] = loc;
  }
  buf->len += len;
  return true;
}

// Purpose: Append a string slice with per-byte source mappings.
// Inputs: buf is the target buffer; s/len describe bytes; map supplies mappings.
// Outputs: Returns true on success and increments buf->len.
// Invariants/Assumptions: map has at least len entries.
static bool buffer_append_str_with_map(struct Buffer* buf, char* s, size_t len,
                                       struct SourceMappingEntry* map) {
  if (!buffer_reserve(buf, len)) return false;
  memcpy(buf->data + buf->len, s, len);
  memcpy(buf->map + buf->len, map, len * sizeof(*buf->map));
  buf->len += len;
  return true;
}

// Purpose: NUL-terminate the buffer content.
// Inputs: buf is the target buffer.
// Outputs: Returns true on success and writes a trailing '\0'.
// Invariants/Assumptions: buffer_reserve ensures space for the terminator.
static bool buffer_finish(struct Buffer* buf) {
  if (!buffer_reserve(buf, 0)) return false;
  buf->data[buf->len] = '\0';
  return true;
}

// Purpose: Test whether a character can start an identifier.
// Inputs: c is the candidate character.
// Outputs: Returns true when c can begin an identifier.
// Invariants/Assumptions: Uses ASCII character classes.
static bool is_ident_start(char c) {
  return isalpha((unsigned char)c) || c == '_';
}

// Purpose: Test whether a character can appear inside an identifier.
// Inputs: c is the candidate character.
// Outputs: Returns true when c is a valid identifier character.
// Invariants/Assumptions: Uses ASCII character classes.
static bool is_ident_char(char c) {
  return isalnum((unsigned char)c) || c == '_';
}

// Purpose: Check if the identifier matches a reserved builtin macro name.
// Inputs: name/len describe the identifier to test.
// Outputs: Returns true when the name is a builtin macro.
// Invariants/Assumptions: name may be non-NUL-terminated.
static bool is_builtin_macro_name(char* name, size_t len) {
  if (len == K_BUILTIN_FILE_MACRO_LEN && strncmp(name, kBuiltinFileMacro, K_BUILTIN_FILE_MACRO_LEN) == 0) {
    return true;
  }
  if (len == K_BUILTIN_LINE_MACRO_LEN && strncmp(name, kBuiltinLineMacro, K_BUILTIN_LINE_MACRO_LEN) == 0) {
    return true;
  }
  return false;
}

// Purpose: Look up a macro by name in the linked list.
// Inputs: macros is the head of the list; name/len describe the lookup key.
// Outputs: Returns a pointer to the macro or NULL if not found.
// Invariants/Assumptions: Macro names are stored as NUL-terminated copies.
static struct Macro* macro_find(struct Macro* macros, char* name, size_t len) {
  for (struct Macro* macro = macros; macro != NULL; macro = macro->next) {
    if (macro->name_len == len && strncmp(macro->name, name, len) == 0) {
      return macro;
    }
  }
  return NULL;
}

// Purpose: Check whether a macro name is defined either as builtin or user macro.
// Inputs: macros is the list head; name/len describe the lookup key.
// Outputs: Returns true when a macro is considered defined.
// Invariants/Assumptions: Builtin macros are always treated as defined.
static bool is_macro_defined(struct Macro* macros, char* name, size_t len) {
  if (is_builtin_macro_name(name, len)) return true;
  return macro_find(macros, name, len) != NULL;
}

// Purpose: Define or replace an object-like macro with a raw string value.
// Inputs: macros is the list head; name/value define the macro contents.
// Outputs: Returns true on success and updates the list in place.
// Invariants/Assumptions: Names are treated as case-sensitive byte strings.
static bool macro_define(struct Macro** macros, char* name, size_t name_len, char* value, size_t value_len) {
  struct Macro* existing = macro_find(*macros, name, name_len);
  char* value_copy = malloc(value_len + 1);
  if (value_copy == NULL) return false;
  if (value_len > 0) memcpy(value_copy, value, value_len);
  value_copy[value_len] = '\0';

  if (existing != NULL) {
    free(existing->value);
    existing->value = value_copy;
    existing->value_len = value_len;
    return true;
  }

  struct Macro* macro = malloc(sizeof(struct Macro));
  if (macro == NULL) {
    free(value_copy);
    return false;
  }

  char* name_copy = malloc(name_len + 1);
  if (name_copy == NULL) {
    free(value_copy);
    free(macro);
    return false;
  }
  memcpy(name_copy, name, name_len);
  name_copy[name_len] = '\0';

  macro->name = name_copy;
  macro->name_len = name_len;
  macro->value = value_copy;
  macro->value_len = value_len;
  macro->next = *macros;
  *macros = macro;
  return true;
}

// Purpose: Free all macro storage owned by the preprocessor.
// Inputs: macros is the head of the linked list.
// Outputs: Releases all heap allocations for macros.
// Invariants/Assumptions: macros was built via macro_define.
static void destroy_macros(struct Macro* macros) {
  while (macros != NULL) {
    struct Macro* next = macros->next;
    free(macros->name);
    free(macros->value);
    free(macros);
    macros = next;
  }
}

// Purpose: Push a new conditional state for #ifdef/#ifndef.
// Inputs: stack is the conditional stack; condition_true is the evaluated result.
// Outputs: Returns true on success and updates stack->current_active.
// Invariants/Assumptions: stack->current_active reflects outer conditionals.
static bool ifstack_push(struct IfStack* stack, bool condition_true) {
  if (stack->count == stack->cap) {
    size_t new_cap = stack->cap == 0 ? 8 : stack->cap * 2;
    struct IfState* next = realloc(stack->items, new_cap * sizeof(struct IfState));
    if (next == NULL) return false;
    stack->items = next;
    stack->cap = new_cap;
  }

  struct IfState state = {stack->current_active, condition_true, false};
  stack->items[stack->count++] = state;
  stack->current_active = state.parent_active && state.condition_true;
  return true;
}

// Purpose: Toggle to the #else branch of the current conditional.
// Inputs: stack is the conditional stack.
// Outputs: Returns true on success and updates stack->current_active.
// Invariants/Assumptions: Each conditional may have at most one #else.
static bool ifstack_else(struct IfStack* stack) {
  if (stack->count == 0) return false;
  struct IfState* state = &stack->items[stack->count - 1];
  if (state->in_else) return false;
  state->in_else = true;
  stack->current_active = state->parent_active && !state->condition_true;
  return true;
}

// Purpose: Pop the current conditional state.
// Inputs: stack is the conditional stack.
// Outputs: Returns true on success and updates stack->current_active.
// Invariants/Assumptions: stack->count tracks nested #if depth.
static bool ifstack_pop(struct IfStack* stack) {
  if (stack->count == 0) return false;
  stack->count--;
  if (stack->count == 0) {
    stack->current_active = true;
  } else {
    struct IfState* state = &stack->items[stack->count - 1];
    stack->current_active = state->parent_active && (state->in_else ? !state->condition_true : state->condition_true);
  }
  return true;
}

// Purpose: Strip comments while preserving strings and character literals.
// Inputs: prog is a NUL-terminated source buffer; filename identifies the source.
// Outputs: Returns true on success and fills out with comment-stripped text and mappings.
// Invariants/Assumptions: Comment delimiters inside strings/chars are ignored.
static bool strip_comments(char* prog, char* filename,
                           struct FileTable* files, struct Buffer* out) {
  char* interned = file_table_intern(files, filename);
  if (interned == NULL) {
    fdputs(STDERR, "Preprocessor memory error\n");
    return false;
  }

  size_t prog_index = 0;
  size_t line = 1;
  size_t column = 1;
  bool in_string = false;
  bool in_char = false;
  bool escape = false;

  while (prog[prog_index] != 0) {
    char c = prog[prog_index];
    struct SourceMappingEntry loc = {interned, line, column};

    if (!in_string && !in_char) {
      // remove single line // comments
      if (c == '/' && prog[prog_index + 1] == '/') {
        if (!buffer_append_char(out, ' ', loc)) goto fail;
        prog_index += 2;
        column += 2;
        while (prog[prog_index] != '\0' && prog[prog_index] != '\n') {
          prog_index++;
          column++;
        }
        if (prog[prog_index] == '\n') {
          struct SourceMappingEntry newline_loc = {interned, line, column};
          if (!buffer_append_char(out, '\n', newline_loc)) goto fail;
          prog_index++;
          line++;
          column = 1;
        }
        continue;
      }

      // remove multi line /* */ comments
      if (c == '/' && prog[prog_index + 1] == '*') {
        if (!buffer_append_char(out, ' ', loc)) goto fail;
        prog_index += 2;
        column += 2;
        while (prog[prog_index] != '\0') {
          if (prog[prog_index] == '*' && prog[prog_index + 1] == '/') {
            prog_index += 2;
            column += 2;
            break;
          }
          if (prog[prog_index] == '\n') {
            struct SourceMappingEntry newline_loc = {interned, line, column};
            if (!buffer_append_char(out, '\n', newline_loc)) goto fail;
            prog_index++;
            line++;
            column = 1;
            continue;
          }
          prog_index++;
          column++;
        }
        continue;
      }

      if (c == '"') {
        in_string = true;
      } else if (c == '\'') {
        in_char = true;
      }
    } else {
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (in_string && c == '"') {
        in_string = false;
      } else if (in_char && c == '\'') {
        in_char = false;
      }
    }

    if (!buffer_append_char(out, c, loc)) goto fail;
    prog_index++;
    if (c == '\n') {
      line++;
      column = 1;
    } else {
      column++;
    }
  }

  if (!buffer_finish(out)) goto fail;
  return true;

fail:
  fdputs(STDERR, "Preprocessor memory error\n");
  return false;
}

// Purpose: Read an entire file into a newly allocated buffer.
// Inputs: path is the target file; include_from/line_no identify the include site.
// Outputs: Returns a NUL-terminated buffer or NULL on failure.
// Invariants/Assumptions: Caller owns the returned buffer and must free it.
static char* read_file(char* path, char* include_from, size_t line_no) {
  FILE* file = fopen(path, "rb");
  if (file == NULL) {
    preprocessor_error1_str(include_from, line_no,
                            "Preprocessor error at %s:%zu: failed to open include file: %s\n",
                            path);
    return NULL;
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    preprocessor_error1_str(include_from, line_no,
                            "Preprocessor error at %s:%zu: failed to seek include file: %s\n",
                            path);
    fclose(file);
    return NULL;
  }
  int size = ftell(file);
  if (size < 0) {
    preprocessor_error1_str(include_from, line_no,
                            "Preprocessor error at %s:%zu: failed to size include file: %s\n",
                            path);
    fclose(file);
    return NULL;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    preprocessor_error1_str(include_from, line_no,
                            "Preprocessor error at %s:%zu: failed to seek include file: %s\n",
                            path);
    fclose(file);
    return NULL;
  }

  char* buffer = malloc((size_t)size + 1);
  if (buffer == NULL) {
    preprocessor_error1_str(include_from, line_no,
                            "Preprocessor error at %s:%zu: memory error while reading include file: %s\n",
                            path);
    fclose(file);
    return NULL;
  }

  size_t read = fread(buffer, 1, (size_t)size, file);
  if (read != (size_t)size) {
    preprocessor_error1_str(include_from, line_no,
                            "Preprocessor error at %s:%zu: failed to read include file: %s\n",
                            path);
    free(buffer);
    fclose(file);
    return NULL;
  }
  buffer[size] = '\0';
  fclose(file);
  return buffer;
}

// Purpose: Heap-allocate a copy of the provided string.
// Inputs: src is a NUL-terminated string.
// Outputs: Returns a heap-allocated duplicate or NULL on failure.
// Invariants/Assumptions: Caller owns the returned string.
static char* copy_string(char* src) {
  size_t len = strlen(src);
  char* out = malloc(len + 1);
  if (out == NULL) return NULL;
  memcpy(out, src, len);
  out[len] = '\0';
  return out;
}

// Purpose: Initialize a file table for storing unique filename strings.
// Inputs: table is the table to initialize.
// Outputs: Zeroes fields to a known empty state.
// Invariants/Assumptions: Caller manages lifetime via file_table_destroy.
static void file_table_init(struct FileTable* table) {
  table->names = NULL;
  table->count = 0;
  table->cap = 0;
}

// Purpose: Intern a filename string and return a stable pointer.
// Inputs: table is the interning table; name is the filename to intern.
// Outputs: Returns a pointer to a stored copy or NULL on allocation failure.
// Invariants/Assumptions: Returned pointer remains valid until file_table_destroy.
static char* file_table_intern(struct FileTable* table, char* name) {
  for (size_t i = 0; i < table->count; ++i) {
    if (strcmp(table->names[i], name) == 0) {
      return table->names[i];
    }
  }

  size_t new_cap = table->cap == 0 ? FILE_TABLE_INITIAL_CAP : table->cap;
  if (table->count == table->cap) {
    while (new_cap <= table->count) new_cap *= 2;
    char** next = realloc(table->names, new_cap * sizeof(*table->names));
    if (next == NULL) return NULL;
    table->names = next;
    table->cap = new_cap;
  }

  char* copy = copy_string(name);
  if (copy == NULL) return NULL;
  table->names[table->count++] = copy;
  return copy;
}

// Purpose: Release all filenames stored in the file table.
// Inputs: table was initialized by file_table_init.
// Outputs: Frees all heap allocations and zeroes fields.
// Invariants/Assumptions: Safe to call on empty tables.
static void file_table_destroy(struct FileTable* table) {
  for (size_t i = 0; i < table->count; ++i) {
    free(table->names[i]);
  }
  free(table->names);
  table->names = NULL;
  table->count = 0;
  table->cap = 0;
}

// Purpose: Build one absolute or rooted include path from a directory prefix.
// Inputs: dir is the directory prefix; include_name is the raw include string.
// Outputs: Returns a heap-allocated resolved path.
// Invariants/Assumptions: include_name is relative when this helper is used.
static char* prefix_include_path(char* dir, char* include_name) {
  size_t dir_len = strlen(dir);
  size_t inc_len = strlen(include_name);
  char* out = malloc(dir_len + 1 + inc_len + 1);
  if (out == NULL) return NULL;
  memcpy(out, dir, dir_len);
  out[dir_len] = '/';
  memcpy(out + dir_len + 1, include_name, inc_len);
  out[dir_len + 1 + inc_len] = '\0';
  return out;
}

// Purpose: Resolve include paths for quoted and angle-bracket include directives.
// Inputs: current_file is the including file; include_name is the raw include
//         string; system_include selects /crt lookup for <...> directives.
// Outputs: Returns a heap-allocated resolved path.
// Invariants/Assumptions: Absolute paths are passed through unchanged.
static char* resolve_include_path(char* current_file, char* include_name,
                                  bool system_include) {
  if (include_name[0] == '/') return copy_string(include_name);
  if (system_include) return prefix_include_path(SYSTEM_INCLUDE_ROOT, include_name);

  char* slash = strrchr(current_file, '/');
  if (slash == NULL) return copy_string(include_name);

  size_t dir_len = span_len(current_file, slash);
  char* out = malloc(dir_len + 1);
  if (out == NULL) return NULL;
  memcpy(out, current_file, dir_len);
  out[dir_len] = '\0';

  char* resolved = prefix_include_path(out, include_name);
  free(out);
  return resolved;
}

// Purpose: Validate -D and #define names.
// Inputs: name/len describe the candidate identifier.
// Outputs: Returns true when the name is a valid identifier.
// Invariants/Assumptions: Uses the preprocessor's identifier rules.
static bool is_valid_macro_name(char* name, size_t len) {
  if (len == 0 || !is_ident_start(name[0])) return false;
  for (size_t i = 1; i < len; ++i) {
    if (!is_ident_char(name[i])) return false;
  }
  return true;
}

// Purpose: Apply -DNAME or -DNAME=value definitions from the command line.
// Inputs: macros is the macro list head; defines is the CLI array.
// Outputs: Returns true on success and updates the macro list.
// Invariants/Assumptions: defines entries are NUL-terminated strings.
static bool apply_cli_defines(struct Macro** macros, int num_defines, char* * defines) {
  for (int i = 0; i < num_defines; ++i) {
    int args[2];
    char* def = defines[i];
    if (def == NULL || def[0] == '\0') {
      fdputs(STDERR, "Invalid -D definition\n");
      return false;
    }

    char* eq = strchr(def, '=');
    size_t name_len = eq == NULL ? strlen(def) : span_len(def, eq);
    if (!is_valid_macro_name(def, name_len)) {
      args[0] = (int)name_len;
      args[1] = (int)def;
      fdprintf(STDERR, "Invalid -D name: %.*s\n", args);
      return false;
    }
    if (is_builtin_macro_name(def, name_len)) {
      args[0] = (int)name_len;
      args[1] = (int)def;
      fdprintf(STDERR, "Invalid -D name: %.*s (reserved builtin macro)\n", args);
      return false;
    }

    char* value = eq == NULL ? "1" : eq + 1;
    size_t value_len = eq == NULL ? 1 : strlen(eq + 1);
    if (!macro_define(macros, def, name_len, value, value_len)) {
      fdputs(STDERR, "Preprocessor memory error\n");
      return false;
    }
  }
  return true;
}

// Purpose: Append a size_t as a decimal string.
// Inputs: out is the target buffer; line_no is the numeric value to append; loc is the source mapping.
// Outputs: Returns true on success and appends the decimal digits.
// Invariants/Assumptions: Uses LINE_NUMBER_BUFFER_CAP as a safe upper bound.
static bool buffer_append_line_number(struct Buffer* out, size_t line_no, struct SourceMappingEntry loc) {
  char digits[LINE_NUMBER_BUFFER_CAP];
  size_t len = 0;
  size_t value = line_no;

  do {
    digits[len++] = (char)('0' + (value % 10));
    value /= 10;
  } while (value != 0);

  for (size_t i = 0; i < len; ++i) {
    if (!buffer_append_char(out, digits[len - 1 - i], loc)) return false;
  }
  return true;
}

// Purpose: Append a C string literal with minimal escaping.
// Inputs: out is the target buffer; value is the raw string to quote; loc is the source mapping.
// Outputs: Returns true on success and appends a quoted string literal.
// Invariants/Assumptions: Escapes backslashes and double quotes only.
static bool buffer_append_c_string_literal(struct Buffer* out, char* value, struct SourceMappingEntry loc) {
  if (!buffer_append_char(out, '"', loc)) return false;
  for (char* p = value; *p != '\0'; ++p) {
    if (*p == '\\' || *p == '"') {
      if (!buffer_append_char(out, '\\', loc)) return false;
    }
    if (!buffer_append_char(out, *p, loc)) return false;
  }
  return buffer_append_char(out, '"', loc);
}

// Purpose: Expand builtin macros that depend on file/line context.
// Inputs: name/len describe the identifier; loc provides file/line context.
// Outputs: Sets *matched when a builtin is expanded and appends to out.
// Invariants/Assumptions: Builtins are object-like and non-recursive.
static bool try_expand_builtin_macro(char* name, size_t len,
                                     struct SourceMappingEntry* loc,
                                     struct Buffer* out, bool* matched) {
  *matched = false;
  if (len == K_BUILTIN_FILE_MACRO_LEN && strncmp(name, kBuiltinFileMacro, K_BUILTIN_FILE_MACRO_LEN) == 0) {
    *matched = true;
    return buffer_append_c_string_literal(out, loc->filename, *loc);
  }
  if (len == K_BUILTIN_LINE_MACRO_LEN && strncmp(name, kBuiltinLineMacro, K_BUILTIN_LINE_MACRO_LEN) == 0) {
    *matched = true;
    return buffer_append_line_number(out, loc->line, *loc);
  }
  return true;
}

// Purpose: Expand macros in a single line, skipping strings and char literals.
// Inputs: line/len describe the line; line_map provides source mappings; macros is the macro list.
// Outputs: Returns true on success and appends expanded content to out.
// Invariants/Assumptions: Macro replacement is non-recursive and object-like only.
static bool expand_macros_in_line(char* line, size_t len,
                                  struct SourceMappingEntry* line_map,
                                  struct Macro* macros, struct Buffer* out) {
  bool in_string = false;
  bool in_char = false;
  bool escape = false;

  for (size_t i = 0; i < len; ) {
    char c = line[i];
    struct SourceMappingEntry loc = line_map[i];

    if (in_string) {
      if (!buffer_append_char(out, c, loc)) return false;
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        in_string = false;
      }
      i++;
      continue;
    }

    if (in_char) {
      if (!buffer_append_char(out, c, loc)) return false;
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '\'') {
        in_char = false;
      }
      i++;
      continue;
    }

    if (c == '"') {
      in_string = true;
      if (!buffer_append_char(out, c, loc)) return false;
      i++;
      continue;
    }
    if (c == '\'') {
      in_char = true;
      if (!buffer_append_char(out, c, loc)) return false;
      i++;
      continue;
    }

    if (is_ident_start(c)) {
      size_t start = i;
      i++;
      while (i < len && is_ident_char(line[i])) i++;
      bool matched = false;
      struct SourceMappingEntry macro_loc = line_map[start];
      if (!try_expand_builtin_macro(line + start, i - start, &macro_loc, out, &matched)) {
        return false;
      }
      if (matched) continue;
      struct Macro* macro = macro_find(macros, line + start, i - start);
      if (macro != NULL) {
        if (!buffer_append_str_with_loc(out, macro->value, macro->value_len, macro_loc)) return false;
      } else {
        if (!buffer_append_str_with_map(out, line + start, i - start, line_map + start)) return false;
      }
      continue;
    }

    if (!buffer_append_char(out, c, loc)) return false;
    i++;
  }

  return true;
}

// Purpose: Parse a #define line (object-like only) and store it in the macro list.
// Inputs: line/line_end are the directive contents; filename/line_no for diagnostics.
// Outputs: Returns true on success and updates the macro list.
// Invariants/Assumptions: Function-like macros are not supported.
static bool parse_define_line(char* line, char* line_end,
                              char* filename, size_t line_no,
                              struct Macro** macros) {
  char* p = line;
  while (p < line_end && isspace((unsigned char)*p)) p++;
  if (p >= line_end || !is_ident_start(*p)) {
    preprocessor_error0(filename, line_no, "invalid #define directive");
    return false;
  }

  char* name_start = p;
  p++;
  while (p < line_end && is_ident_char(*p)) p++;
  size_t name_len = span_len(name_start, p);
  if (is_builtin_macro_name(name_start, name_len)) {
    preprocessor_error1_slice(filename, line_no,
                              "Preprocessor error at %s:%zu: cannot redefine builtin macro: %.*s\n",
                              name_len, name_start);
    return false;
  }

  while (p < line_end && isspace((unsigned char)*p)) p++;
  char* value_start = p;
  char* value_end = line_end;
  while (value_end > value_start && isspace((unsigned char)*(value_end - 1))) value_end--;

  if (!macro_define(macros, name_start, name_len, value_start, span_len(value_start, value_end))) {
    preprocessor_error0(filename, line_no, "memory error while defining macro");
    return false;
  }
  return true;
}

// Purpose: Preprocess a single source buffer (shared macro state).
// Inputs: prog/filename identify the source; macros is the shared macro list.
// Outputs: Returns a newly allocated preprocessed buffer or NULL on failure.
// Invariants/Assumptions: The macros list is owned by the caller.
static bool preprocess_buffer(char* prog, char* filename,
                              struct Macro** macros, struct FileTable* files,
                              struct PreprocessOutput* out);

// Purpose: Handle #include "path" and #include <path> by preprocessing the
//          included file in place.
// Inputs: line/line_end describe the directive contents; macros/out are updated.
// Outputs: Returns true on success and appends included text to out.
// Invariants/Assumptions: Quoted includes resolve relative to the current file;
//                         angle-bracket includes resolve under /crt.
static bool handle_include_line(char* line, char* line_end, char* filename,
                                size_t line_no, struct Macro** macros, struct FileTable* files,
                                struct Buffer* out, struct SourceMappingEntry newline_loc,
                                bool add_newline) {
  char* p = line;
  char end_delim;
  bool system_include;
  while (p < line_end && isspace((unsigned char)*p)) p++;
  if (p >= line_end || (*p != '"' && *p != '<')) {
    preprocessor_error0(filename, line_no, "invalid #include directive");
    return false;
  }
  system_include = (*p == '<');
  end_delim = system_include ? '>' : '"';
  p++;
  char* name_start = p;
  while (p < line_end && *p != end_delim) p++;
  if (p >= line_end) {
    preprocessor_error0(filename, line_no, "invalid #include directive");
    return false;
  }
  size_t name_len = span_len(name_start, p);

  char* include_name = malloc(name_len + 1);
  if (include_name == NULL) {
    preprocessor_error0(filename, line_no, "memory error while parsing include directive");
    return false;
  }
  memcpy(include_name, name_start, name_len);
  include_name[name_len] = '\0';

  char* include_path = resolve_include_path(filename, include_name, system_include);
  free(include_name);
  if (include_path == NULL) {
    preprocessor_error0(filename, line_no, "memory error while resolving include path");
    return false;
  }

  char* include_source = read_file(include_path, filename, line_no);
  if (include_source == NULL) {
    free(include_path);
    return false;
  }

  struct PreprocessOutput include_output = {0};
  if (!preprocess_buffer(include_source, include_path, macros, files, &include_output)) {
    free(include_source);
    free(include_path);
    return false;
  }
  free(include_source);
  free(include_path);
  size_t include_len = include_output.map.length;
  bool ok = buffer_append_str_with_map(out, include_output.text, include_len, include_output.map.entries);
  if (ok && add_newline && (include_len == 0 || include_output.text[include_len - 1] != '\n')) {
    ok = buffer_append_char(out, '\n', newline_loc);
  }
  free(include_output.text);
  free(include_output.map.entries);
  return ok;
}

// Purpose: Parse and validate the identifier in #ifdef/#ifndef directives.
// Inputs: line/line_end describe the directive; directive names the keyword.
// Outputs: Returns true on success and fills name_start/name_len.
// Invariants/Assumptions: Only identifiers are accepted (no expressions).
static bool parse_ifdef_name(char* line, char* line_end,
                             char* filename, size_t line_no,
                             char* directive,
                             char** name_start, size_t* name_len) {
  char* p = line;
  while (p < line_end && isspace((unsigned char)*p)) p++;
  if (p >= line_end || !is_ident_start(*p)) {
    preprocessor_error1_str(filename, line_no,
                            "Preprocessor error at %s:%zu: invalid #%s directive\n",
                            directive);
    return false;
  }
  char* start = p;
  p++;
  while (p < line_end && is_ident_char(*p)) p++;
  *name_start = start;
  *name_len = span_len(start, p);
  return true;
}

// Purpose: Parse a "defined" term used in #if expressions.
// Inputs: line/line_end describe the directive contents.
// Outputs: Returns true on success and fills out_value/next.
// Invariants/Assumptions: Supports optional leading '!' and optional parentheses.
static bool parse_if_defined_term(char* line, char* line_end,
                                  char* filename, size_t line_no,
                                  struct Macro* macros,
                                  bool* out_value, char** next) {
  char* p = line;
  while (p < line_end && isspace((unsigned char)*p)) p++;
  bool negate = false;
  if (p < line_end && *p == '!') {
    negate = true;
    p++;
    while (p < line_end && isspace((unsigned char)*p)) p++;
  }

  if (span_len(p, line_end) < 7 || strncmp(p, "defined", 7) != 0) {
    preprocessor_error0(filename, line_no, "invalid #if directive (expected defined)");
    return false;
  }
  p += 7;
  while (p < line_end && isspace((unsigned char)*p)) p++;

  bool has_paren = false;
  if (p < line_end && *p == '(') {
    has_paren = true;
    p++;
    while (p < line_end && isspace((unsigned char)*p)) p++;
  }

  if (p >= line_end || !is_ident_start(*p)) {
    preprocessor_error0(filename, line_no, "invalid #if directive (expected identifier)");
    return false;
  }
  char* name_start = p;
  p++;
  while (p < line_end && is_ident_char(*p)) p++;
  size_t name_len = span_len(name_start, p);

  while (p < line_end && isspace((unsigned char)*p)) p++;
  if (has_paren) {
    if (p >= line_end || *p != ')') {
      preprocessor_error0(filename, line_no, "invalid #if directive (missing ')')");
      return false;
    }
    p++;
  }

  bool is_defined = is_macro_defined(macros, name_start, name_len);
  *out_value = negate ? !is_defined : is_defined;
  *next = p;
  return true;
}

// Purpose: Parse a limited #if condition with defined/!defined and &&.
// Inputs: line/line_end describe the directive contents.
// Outputs: Returns true on success and fills out_value.
// Invariants/Assumptions: Only supports AND-combined defined checks.
static bool parse_if_condition(char* line, char* line_end,
                               char* filename, size_t line_no,
                               struct Macro* macros, bool* out_value) {
  char* p = line;
  bool value = false;
  if (!parse_if_defined_term(p, line_end, filename, line_no, macros, &value, &p)) {
    return false;
  }

  while (true) {
    while (p < line_end && isspace((unsigned char)*p)) p++;
    if (p + 1 < line_end && p[0] == '&' && p[1] == '&') {
      p += 2;
      bool rhs = false;
      if (!parse_if_defined_term(p, line_end, filename, line_no, macros, &rhs, &p)) {
        return false;
      }
      value = value && rhs;
      continue;
    }
    break;
  }

  while (p < line_end && isspace((unsigned char)*p)) p++;
  if (p != line_end) {
    preprocessor_error0(filename, line_no, "invalid #if directive (unexpected tokens)");
    return false;
  }

  *out_value = value;
  return true;
}

// Purpose: Process a single preprocessor directive line.
// Inputs: line_start/line_end span the directive; if_stack tracks conditionals.
// Outputs: Returns true on success and updates macros/output buffers as needed.
// Invariants/Assumptions: Caller ensures line_start points just after '#'.
static bool preprocess_directive(
    char* line_start,
    char* line_end,
    struct SourceMappingEntry newline_loc,
    bool has_newline,
    size_t line_no,
    char* filename,
    struct Macro** macros,
    struct FileTable* files,
    struct Buffer* out,
    struct IfStack* if_stack) {
  char* p = line_start;
  while (p < line_end && isspace((unsigned char)*p)) p++;
  char* word_start = p;
  while (p < line_end && isalpha((unsigned char)*p)) p++;
  size_t word_len = span_len(word_start, p);

  if (word_len == 0) {
    preprocessor_error0(filename, line_no, "invalid preprocessor directive");
    return false;
  }

  bool is_active = if_stack->current_active;

  if (word_len == 7 && strncmp(word_start, "include", 7) == 0) {
    if (!is_active) return true;
    return handle_include_line(p, line_end, filename, line_no, macros, files,
                               out, newline_loc, has_newline);
  }

  if (word_len == 6 && strncmp(word_start, "define", 6) == 0) {
    if (!is_active) return true;
    return parse_define_line(p, line_end, filename, line_no, macros);
  }

  if (word_len == 2 && strncmp(word_start, "if", 2) == 0) {
    bool condition_true = false;
    if (if_stack->current_active) {
      if (!parse_if_condition(p, line_end, filename, line_no, *macros, &condition_true)) {
        return false;
      }
    } else {
      bool ignored = false;
      if (!parse_if_condition(p, line_end, filename, line_no, *macros, &ignored)) {
        return false;
      }
      condition_true = false;
    }
    if (!ifstack_push(if_stack, condition_true)) {
      fdputs(STDERR, "Preprocessor memory error\n");
      return false;
    }
    return true;
  }

  if (word_len == 5 && strncmp(word_start, "ifdef", 5) == 0) {
    char* name_start = NULL;
    size_t name_len = 0;
    if (!parse_ifdef_name(p, line_end, filename, line_no, "ifdef", &name_start, &name_len)) {
      return false;
    }
    bool condition_true = false;
    if (if_stack->current_active) {
      condition_true = is_macro_defined(*macros, name_start, name_len);
    }
    if (!ifstack_push(if_stack, condition_true)) {
      fdputs(STDERR, "Preprocessor memory error\n");
      return false;
    }
    return true;
  }

  if (word_len == 6 && strncmp(word_start, "ifndef", 6) == 0) {
    char* name_start = NULL;
    size_t name_len = 0;
    if (!parse_ifdef_name(p, line_end, filename, line_no, "ifndef", &name_start, &name_len)) {
      return false;
    }
    bool condition_true = false;
    if (if_stack->current_active) {
      condition_true = !is_macro_defined(*macros, name_start, name_len);
    }
    if (!ifstack_push(if_stack, condition_true)) {
      fdputs(STDERR, "Preprocessor memory error\n");
      return false;
    }
    return true;
  }

  if (word_len == 6 && strncmp(word_start, "pragma", 6) == 0) {
    // Ignore pragmas in this minimal preprocessor.
    return true;
  }

  if (word_len == 4 && strncmp(word_start, "else", 4) == 0) {
    if (!ifstack_else(if_stack)) {
      preprocessor_error0(filename, line_no, "unexpected #else");
      return false;
    }
    return true;
  }

  if (word_len == 5 && strncmp(word_start, "endif", 5) == 0) {
    if (!ifstack_pop(if_stack)) {
      preprocessor_error0(filename, line_no, "unexpected #endif");
      return false;
    }
    return true;
  }

  preprocessor_error0(filename, line_no, "unknown preprocessor directive");
  return false;
}

// Purpose: Strip comments, apply directives, and expand macros line-by-line.
// Inputs: prog/filename identify the source; macros is the shared macro list.
// Outputs: Returns true on success and fills out with preprocessed data.
// Invariants/Assumptions: Conditional blocks only use #ifdef/#ifndef/#else/#endif.
static bool preprocess_buffer(char* prog, char* filename,
                              struct Macro** macros, struct FileTable* files,
                              struct PreprocessOutput* out) {
  struct Buffer no_comments;
  size_t initial_cap = strlen(prog) + 1;
  if (initial_cap < 64) initial_cap = 64;
  if (!buffer_init(&no_comments, initial_cap)) {
    fdputs(STDERR, "Preprocessor memory error\n");
    return false;
  }
  if (!strip_comments(prog, filename, files, &no_comments)) {
    free(no_comments.data);
    free(no_comments.map);
    return false;
  }

  struct Buffer output;
  size_t output_cap = no_comments.len + 1;
  if (output_cap < 64) output_cap = 64;
  if (!buffer_init(&output, output_cap)) {
    fdputs(STDERR, "Preprocessor memory error\n");
    free(no_comments.data);
    free(no_comments.map);
    return false;
  }

  // Track nesting and whether we're currently emitting active regions.
  struct IfStack if_stack;
  if_stack.items = NULL;
  if_stack.count = 0;
  if_stack.cap = 0;
  if_stack.current_active = true;

  char* cursor = no_comments.data;
  size_t line_no = 1;
  while (*cursor != '\0') {
    char* line_start = cursor;
    while (*cursor != '\0' && *cursor != '\n') cursor++;
    char* line_end = cursor;
    bool has_newline = (*cursor == '\n');
    if (has_newline) cursor++;
    size_t line_len = span_len(line_start, line_end);
    size_t line_offset = span_len(no_comments.data, line_start);
    struct SourceMappingEntry* line_map = no_comments.map + line_offset;
    struct SourceMappingEntry newline_loc = {NULL, 0, 0};
    if (has_newline) {
      newline_loc = no_comments.map[line_offset + line_len];
    }

    char* p = line_start;
    while (p < line_end && isspace((unsigned char)*p)) p++;
    // Directive lines are recognized even when indented.
    if (p < line_end && *p == '#') {
      if (!preprocess_directive(p + 1, line_end, newline_loc, has_newline, line_no,
                                filename, macros, files, &output, &if_stack)) {
        free(no_comments.data);
        free(no_comments.map);
        free(output.data);
        free(output.map);
        free(if_stack.items);
        return false;
      }
      if (has_newline) line_no++;
      continue;
    }

    // Only emit non-directive lines from active regions.
    if (if_stack.current_active) {
      if (!expand_macros_in_line(line_start, line_len, line_map, *macros, &output)) {
        fdputs(STDERR, "Preprocessor memory error\n");
        free(no_comments.data);
        free(no_comments.map);
        free(output.data);
        free(output.map);
        free(if_stack.items);
        return false;
      }
      if (has_newline && !buffer_append_char(&output, '\n', newline_loc)) {
        fdputs(STDERR, "Preprocessor memory error\n");
        free(no_comments.data);
        free(no_comments.map);
        free(output.data);
        free(output.map);
        free(if_stack.items);
        return false;
      }
    }
    if (has_newline) line_no++;
  }

  // Unterminated #ifdef/#ifndef should be reported as an error.
  if (if_stack.count != 0) {
    preprocessor_error0(filename, line_no,
                        "unterminated #ifdef/#ifndef block (reached end of file)");
    free(no_comments.data);
    free(no_comments.map);
    free(output.data);
    free(output.map);
    free(if_stack.items);
    return false;
  }

  free(no_comments.data);
  free(no_comments.map);
  free(if_stack.items);
  if (!buffer_finish(&output)) {
    fdputs(STDERR, "Preprocessor memory error\n");
    free(output.data);
    free(output.map);
    return false;
  }
  out->text = output.data;
  out->map.entries = output.map;
  out->map.length = output.len;
  return true;
}

// Purpose: Public entrypoint: apply CLI defines, then preprocess the buffer.
// Inputs: prog is the source buffer; filename is used for diagnostics.
// Outputs: Returns true on success and fills result; false on failure.
// Invariants/Assumptions: Caller must free result via destroy_preprocess_result.
bool preprocess(char * prog, char* filename, int num_defines,
                char* * defines, struct PreprocessResult* result) {
  if (result == NULL) return false;
  result->text = NULL;
  result->map.entries = NULL;
  result->map.length = 0;
  file_table_init(&result->file_table);

  struct Macro* macros = NULL;
  if (!apply_cli_defines(&macros, num_defines, defines)) {
    destroy_macros(macros);
    file_table_destroy(&result->file_table);
    return false;
  }

  struct PreprocessOutput output = {0};
  if (!preprocess_buffer(prog, filename, &macros, &result->file_table, &output)) {
    destroy_macros(macros);
    file_table_destroy(&result->file_table);
    return false;
  }

  destroy_macros(macros);
  result->text = output.text;
  result->map = output.map;
  return true;
}

// Purpose: Free all storage owned by a PreprocessResult.
// Inputs: result was filled by preprocess.
// Outputs: Releases heap allocations and zeroes pointers/counts.
// Invariants/Assumptions: Safe to call with partially initialized results.
void destroy_preprocess_result(struct PreprocessResult* result) {
  if (result == NULL) return;
  free(result->text);
  free(result->map.entries);
  result->text = NULL;
  result->map.entries = NULL;
  result->map.length = 0;
  file_table_destroy(&result->file_table);
}
