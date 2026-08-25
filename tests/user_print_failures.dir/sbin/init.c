/*
 * CRT formatted-output return-value regression.
 *
 * Validates:
 * - mixed literals, numbers, padding, strings, characters, percent escaping,
 *   and an unknown conversion produce both the exact bytes and exact count
 * - printf/puts report their normal stdout byte counts
 * - invalid and already-closed descriptors report failure
 * - fputs/fwrite preserve their distinct failure contracts on a pipe whose
 *   final reader has closed
 * - stdout-only character/numeric helpers report failure without disturbing
 *   the parent's inherited stdout descriptor
 */

#include "../../../root/crt/limits.h"
#include "../../../root/crt/print.h"
#include "../../../root/crt/stdio.h"
#include "../../../root/crt/string.h"
#include "../../../root/crt/sys.h"
#include "../../user_test.h"

#define FORMATTED_EXPECTED "lit:-0042|17|0000002a|ABCD|hello|wor|Z|%|%q"
#define FORMATTED_EXPECTED_BYTES 43
#define FORMATTED_BUFFER_BYTES 44

#define CLOSED_STDOUT_CHILD_OK 71
#define CLOSED_STDOUT_CLOSE_FAILED 72
#define CLOSED_STDOUT_PRINTF_FAILED 73
#define CLOSED_STDOUT_PUTS_FAILED 74
#define CLOSED_STDOUT_PUTCHAR_FAILED 75
#define CLOSED_STDOUT_SIGNED_FAILED 76
#define CLOSED_STDOUT_UNSIGNED_FAILED 77
#define CLOSED_STDOUT_HEX_FAILED 78

static int bytes_equal(char* lhs, char* rhs, unsigned count){
  for (unsigned i = 0; i < count; ++i){
    if (lhs[i] != rhs[i]){
      return 0;
    }
  }
  return 1;
}

static int closed_stdout_child(void){
  if (close(STDOUT) != 0){
    return CLOSED_STDOUT_CLOSE_FAILED;
  }
  if (printf("x", NULL) != -1){
    return CLOSED_STDOUT_PRINTF_FAILED;
  }
  if (puts("x") != -1){
    return CLOSED_STDOUT_PUTS_FAILED;
  }
  if (putchar('x') != -1){
    return CLOSED_STDOUT_PUTCHAR_FAILED;
  }
  if (print_signed(-1) != -1){
    return CLOSED_STDOUT_SIGNED_FAILED;
  }
  if (print_unsigned(1) != -1){
    return CLOSED_STDOUT_UNSIGNED_FAILED;
  }
  if (print_hex(1, false) != -1){
    return CLOSED_STDOUT_HEX_FAILED;
  }
  return CLOSED_STDOUT_CHILD_OK;
}

int main(void){
  int format_args[8];
  char actual[FORMATTED_BUFFER_BYTES];
  int pipe_fds[2];
  int stdout_args[1];
  FILE formatted_stream;
  FILE pipe_stream;
  int fd;
  int rc;
  int child;

  format_args[0] = -42;
  format_args[1] = 17;
  format_args[2] = 42;
  format_args[3] = 0xABCD;
  format_args[4] = (int)"hello";
  format_args[5] = 3;
  format_args[6] = (int)"world";
  format_args[7] = 'Z';

  fd = open("print-output");
  user_test_expect_eq("open formatted-output file", fd >= 0, 1);
  rc = fdprintf(fd, "lit:%05d|%u|%08x|%X|%s|%.*s|%c|%%|%q",
                format_args);
  user_test_expect_eq("fdprintf reports exact mixed-format byte count", rc,
                      FORMATTED_EXPECTED_BYTES);
  user_test_expect_eq("rewind formatted-output file",
                      seek(fd, 0, SEEK_SET), 0);
  rc = read(fd, actual, FORMATTED_EXPECTED_BYTES);
  user_test_expect_eq("read exact mixed-format byte count", rc,
                      FORMATTED_EXPECTED_BYTES);
  if (rc == FORMATTED_EXPECTED_BYTES){
    actual[FORMATTED_EXPECTED_BYTES] = '\0';
  } else {
    actual[0] = '\0';
  }
  user_test_expect_eq("fdprintf emitted exact mixed-format bytes",
                      bytes_equal(actual, FORMATTED_EXPECTED,
                                  FORMATTED_EXPECTED_BYTES), 1);

  // The mathematical product does not fit the CRT's 32-bit size_t. Both stdio
  // operations must reject it before dereferencing the deliberately undersized
  // buffer or issuing descriptor I/O.
  formatted_stream = fd;
  user_test_expect_eq("fwrite rejects wrapped item extent",
                      fwrite(actual, UINT_MAX, 2, &formatted_stream), 0);
  user_test_expect_eq("rejected wrapped fwrite preserves file offset",
                      ftell(&formatted_stream), FORMATTED_EXPECTED_BYTES);
  user_test_expect_eq("fread rejects wrapped item extent",
                      fread(actual, UINT_MAX, 2, &formatted_stream), 0);
  user_test_expect_eq("rejected wrapped fread preserves file offset",
                      ftell(&formatted_stream), FORMATTED_EXPECTED_BYTES);
  user_test_expect_eq("rewind after wrapped stdio requests",
                      fseek(&formatted_stream, 0, SEEK_SET), 0);
  rc = read(fd, actual, FORMATTED_EXPECTED_BYTES);
  user_test_expect_eq("read bytes after wrapped stdio requests", rc,
                      FORMATTED_EXPECTED_BYTES);
  user_test_expect_eq("wrapped stdio requests preserve file contents",
                      bytes_equal(actual, FORMATTED_EXPECTED,
                                  FORMATTED_EXPECTED_BYTES), 1);

  user_test_expect_eq("oversized format width is rejected",
                      fdprintf(fd, "%2147483648d", format_args), -1);
  user_test_expect_eq("close formatted-output file", close(fd), 0);

  user_test_expect_eq("fdprintf rejects invalid descriptor",
                      fdprintf(-1, "literal", NULL), -1);

  fd = open("closed-output");
  user_test_expect_eq("open closed-output file", fd >= 0, 1);
  user_test_expect_eq("close closed-output descriptor", close(fd), 0);
  user_test_expect_eq("fdputs rejects already-closed descriptor",
                      fdputs(fd, "closed"), -1);

  pipe_fds[0] = -1;
  pipe_fds[1] = -1;
  user_test_expect_eq("create output-failure pipe", pipe(pipe_fds), 0);
  user_test_expect_eq("close final output-failure pipe reader",
                      close(pipe_fds[0]), 0);
  pipe_stream = pipe_fds[1];
  user_test_expect_eq("fputs propagates readerless-pipe failure",
                      fputs("pipe", &pipe_stream), -1);
  user_test_expect_eq("fwrite reports zero complete readerless-pipe items",
                      fwrite("pipe", 1, 4, &pipe_stream), 0);
  user_test_expect_eq("close output-failure pipe writer",
                      close(pipe_fds[1]), 0);

  stdout_args[0] = -7;
  user_test_expect_eq("printf reports exact output count",
                      printf("x=%d\n", stdout_args), 5);
  user_test_expect_eq("puts reports exact output count", puts("plain\n"), 6);

  child = fork();
  if (child == 0){
    return closed_stdout_child();
  }
  user_test_expect_eq("closed-stdout helpers report write failure",
                      wait_child(child), CLOSED_STDOUT_CHILD_OK);

  return 0;
}
