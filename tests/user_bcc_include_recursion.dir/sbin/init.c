/*
 * Exercises the bundled preprocessor's active-include safety boundary.
 *
 * The test creates real guest files because include handling intentionally
 * goes through the CRT/filesystem path. It verifies an acyclic chain at the
 * documented maximum, an ordinary mutual cycle, and a lexical-alias cycle
 * whose path spelling changes on every recursion and must therefore terminate
 * through the independent depth bound.
 */

#include "../../../root/crt/stdbool.h"
#include "../../../root/crt/stdio.h"
#include "../../../root/crt/string.h"
#include "../../../root/crt/sys.h"
#include "../../../root/crt/unistd.h"
#include "../../../root/bcc/preprocessor.h"
#include "../../user_test.h"

#define DEPTH_PATH_BUFFER_BYTES 11
#define INCLUDE_LINE_BUFFER_BYTES 23
#define DEPTH_FILE_COUNT 32

static bool write_fixture(char* path, char* contents) {
  int fd = open(path);
  if (fd < 0) return false;

  unsigned length = strlen(contents);
  unsigned written = 0;
  while (written < length) {
    int chunk = write(fd, contents + written, length - written);
    if (chunk <= 0) break;
    written += (unsigned)chunk;
  }

  int truncate_result = written == length ? truncate(fd, length) : -1;
  int close_result = close(fd);
  return written == length && truncate_result == 0 && close_result == 0;
}

// `/depth00.h` is ten bytes plus its terminator. Keeping this construction
// literal avoids depending on formatted output while that CRT is under test.
static void make_depth_path(char* path, unsigned index) {
  path[0] = '/';
  path[1] = 'd';
  path[2] = 'e';
  path[3] = 'p';
  path[4] = 't';
  path[5] = 'h';
  path[6] = (char)('0' + (index / 10));
  path[7] = (char)('0' + (index % 10));
  path[8] = '.';
  path[9] = 'h';
  path[10] = '\0';
}

static void make_include_line(char* line, unsigned next_index) {
  char path[DEPTH_PATH_BUFFER_BYTES];
  make_depth_path(path, next_index);
  memcpy(line, "#include \"", 10);
  memcpy(line + 10, path, 10);
  line[20] = '"';
  line[21] = '\n';
  line[22] = '\0';
}

static bool prepare_depth_chain(void) {
  char path[DEPTH_PATH_BUFFER_BYTES];
  char line[INCLUDE_LINE_BUFFER_BYTES];

  for (unsigned i = 0; i < DEPTH_FILE_COUNT; ++i) {
    make_depth_path(path, i);
    if (i + 1 < DEPTH_FILE_COUNT) {
      make_include_line(line, i + 1);
      if (!write_fixture(path, line)) return false;
    } else if (!write_fixture(path, "int depth_boundary;\n")) {
      return false;
    }
  }
  return true;
}

int main(void) {
  bool fixtures_ok = prepare_depth_chain();
  fixtures_ok = fixtures_ok &&
    write_fixture("/cycle_a.h", "#include \"/cycle_b.h\"\n");
  fixtures_ok = fixtures_ok &&
    write_fixture("/cycle_b.h", "#include \"/cycle_a.h\"\n");
  fixtures_ok = fixtures_ok &&
    write_fixture("/alias.h", "#include \"./alias.h\"\n");
  user_test_expect_eq("prepare include recursion fixtures", fixtures_ok, 1);

  struct PreprocessResult result;
  bool ok = preprocess("#include \"/depth00.h\"\n", "/depth_root.c",
                       0, NULL, &result);
  bool boundary_ok = ok && strcmp(result.text, "int depth_boundary;\n") == 0;
  user_test_expect_eq("maximum documented include depth succeeds",
                      boundary_ok, 1);
  destroy_preprocess_result(&result);

  ok = preprocess("#include \"/cycle_a.h\"\n", "/cycle_root.c",
                  0, NULL, &result);
  user_test_expect_eq("active include cycle is rejected", ok, 0);
  destroy_preprocess_result(&result);

  ok = preprocess("#include \"/alias.h\"\n", "/alias_root.c",
                  0, NULL, &result);
  user_test_expect_eq("lexical alias recursion reaches bounded failure", ok, 0);
  destroy_preprocess_result(&result);

  printf("***Done.\n", NULL);
  return 0;
}
