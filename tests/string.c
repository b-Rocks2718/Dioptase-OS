/*
 * String helper test.
 *
 * Validates:
 * - strlen(), streq(), and strneq() follow the kernel's expected string and
 *   prefix semantics
 * - strncpy() matches the current kernel-specific contract for truncation and
 *   trailing NUL handling
 * - memcpy(), memcpy2(), memcpy4(), and memset() operate on raw bytes without
 *   touching bytes outside the requested range
 *
 * How:
 * - build small fixed byte arrays that cover empty strings, embedded NUL bytes,
 *   prefix comparisons, and truncated copies
 * - compare whole byte regions after strncpy(), memcpy(), memcpy2(),
 *   memcpy4(), and memset() so the test checks untouched bytes as well as the
 *   written range
 * - run each helper in a dedicated function so failures identify the exact API
 */
#include "../kernel/string.h"
#include "../kernel/print.h"
#include "../kernel/debug.h"

// Compare two byte regions and fail if any position differs.
static void assert_byte_region(unsigned char* actual, unsigned char* expected,
  unsigned size, char* failure_message) {
  int mismatch = 0;

  for (unsigned i = 0; i < size; ++i){
    if (actual[i] != expected[i]){
      mismatch = 1;
    }
  }

  assert(mismatch == 0, failure_message);
}

// Check strlen() on empty strings, normal strings, and embedded NUL bytes.
static void check_strlen(void) {
  char embedded_nul[5];

  embedded_nul[0] = 'd';
  embedded_nul[1] = 'i';
  embedded_nul[2] = '\0';
  embedded_nul[3] = 'x';
  embedded_nul[4] = '\0';

  assert(strlen("") == 0,
    "string: strlen should report zero for the empty string.\n");
  assert(strlen("dioptase") == 8,
    "string: strlen should count every character before the trailing NUL.\n");
  assert(strlen(embedded_nul) == 2,
    "string: strlen should stop at the first embedded NUL byte.\n");

  say("***strlen: ok\n", NULL);
}

// Check exact string equality on equal and unequal inputs.
static void check_streq(void) {
  assert(streq("kernel", "kernel"),
    "string: streq should return true for identical strings.\n");
  assert(!streq("kernel", "kern"),
    "string: streq should reject strings with different lengths.\n");
  assert(!streq("kernel", "kernal"),
    "string: streq should reject strings with different bytes.\n");

  say("***streq: ok\n", NULL);
}

// Check prefix equality semantics for several boundary cases.
static void check_strneq(void) {
  assert(strneq("kernel", "kernal", 4),
    "string: strneq should accept equal prefixes shorter than the mismatch.\n");
  assert(!strneq("kernel", "kernal", 6),
    "string: strneq should reject mismatches inside the compared prefix.\n");
  assert(strneq("short", "longer", 0),
    "string: strneq should treat a zero-length comparison as equal.\n");
  assert(!strneq("abc", "ab", 3),
    "string: strneq should reject when one string ends before n and the other does not.\n");
  assert(strneq("abc", "abc", 5),
    "string: strneq should accept identical strings even when n exceeds their length.\n");

  say("***strneq: ok\n", NULL);
}

// Check the kernel's strncpy() behavior for short and truncated copies.
static void check_strncpy(void) {
  char short_dest[5];
  char short_expected[5];
  char long_dest[4];
  char long_expected[4];

  short_dest[0] = 'x';
  short_dest[1] = 'x';
  short_dest[2] = 'x';
  short_dest[3] = 'x';
  short_dest[4] = 'x';

  short_expected[0] = 'h';
  short_expected[1] = 'i';
  short_expected[2] = '\0';
  short_expected[3] = 'x';
  short_expected[4] = 'x';

  long_dest[0] = 'x';
  long_dest[1] = 'x';
  long_dest[2] = 'x';
  long_dest[3] = 'x';

  long_expected[0] = 'a';
  long_expected[1] = 'b';
  long_expected[2] = 'c';
  long_expected[3] = 'x';

  assert(strncpy(short_dest, "hi", 5) == short_dest,
    "string: strncpy should return the original destination pointer.\n");
  assert_byte_region((unsigned char*)short_dest, (unsigned char*)short_expected, 5,
    "string: strncpy should copy the source and write one trailing NUL when the source is shorter than n.\n");

  assert(strncpy(long_dest, "abcdef", 3) == long_dest,
    "string: strncpy should still return the destination pointer for truncated copies.\n");
  assert_byte_region((unsigned char*)long_dest, (unsigned char*)long_expected, 4,
    "string: strncpy should copy exactly n bytes and leave later bytes untouched when truncating.\n");

  say("***strncpy: ok\n", NULL);
}

// Check memcpy() raw byte copies and zero-length behavior.
static void check_memcpy(void) {
  unsigned char source[6];
  unsigned char dest[8];
  unsigned char expected[8];
  unsigned char untouched[4];
  unsigned char untouched_expected[4];

  source[0] = 'A';
  source[1] = 0;
  source[2] = 'B';
  source[3] = 0x7F;
  source[4] = 0xFF;
  source[5] = 'Z';

  dest[0] = 0x11;
  dest[1] = 0x11;
  dest[2] = 0x11;
  dest[3] = 0x11;
  dest[4] = 0x11;
  dest[5] = 0x11;
  dest[6] = 0x11;
  dest[7] = 0x11;

  expected[0] = 0x11;
  expected[1] = 'A';
  expected[2] = 0;
  expected[3] = 'B';
  expected[4] = 0x7F;
  expected[5] = 0xFF;
  expected[6] = 'Z';
  expected[7] = 0x11;

  untouched[0] = 1;
  untouched[1] = 2;
  untouched[2] = 3;
  untouched[3] = 4;

  untouched_expected[0] = 1;
  untouched_expected[1] = 2;
  untouched_expected[2] = 3;
  untouched_expected[3] = 4;

  assert((unsigned char*)memcpy(dest + 1, source, 6) == dest + 1,
    "string: memcpy should return the destination pointer.\n");
  assert_byte_region(dest, expected, 8,
    "string: memcpy should copy raw bytes exactly and leave surrounding bytes untouched.\n");

  assert((unsigned char*)memcpy(untouched, source, 0) == untouched,
    "string: memcpy should return the destination pointer for zero-length copies.\n");
  assert_byte_region(untouched, untouched_expected, 4,
    "string: memcpy should leave the destination unchanged when n is zero.\n");

  say("***memcpy: ok\n", NULL);
}

// Check memcpy2() aligned/even-length copies and zero-length behavior.
static void check_memcpy2(void) {
  unsigned short source[3];
  unsigned short dest[5];
  unsigned short expected[5];
  unsigned short untouched[2];
  unsigned short untouched_expected[2];

  source[0] = 0x1234;
  source[1] = 0xABCD;
  source[2] = 0x00FF;

  dest[0] = 0x7777;
  dest[1] = 0x7777;
  dest[2] = 0x7777;
  dest[3] = 0x7777;
  dest[4] = 0x7777;

  expected[0] = 0x7777;
  expected[1] = 0x1234;
  expected[2] = 0xABCD;
  expected[3] = 0x00FF;
  expected[4] = 0x7777;

  untouched[0] = 0x1357;
  untouched[1] = 0x2468;

  untouched_expected[0] = 0x1357;
  untouched_expected[1] = 0x2468;

  assert((unsigned short*)memcpy2(dest + 1, source, 6) == dest + 1,
    "string: memcpy2 should return the destination pointer.\n");
  assert_byte_region((unsigned char*)dest, (unsigned char*)expected, sizeof(dest),
    "string: memcpy2 should copy aligned 2-byte elements exactly and leave surrounding data untouched.\n");

  assert((unsigned short*)memcpy2(untouched, source, 0) == untouched,
    "string: memcpy2 should return the destination pointer for zero-length copies.\n");
  assert_byte_region((unsigned char*)untouched, (unsigned char*)untouched_expected, sizeof(untouched),
    "string: memcpy2 should leave the destination unchanged when n is zero.\n");

  say("***memcpy2: ok\n", NULL);
}

// Check memcpy4() aligned/multiple-of-4 copies and zero-length behavior.
static void check_memcpy4(void) {
  unsigned source[3];
  unsigned dest[5];
  unsigned expected[5];
  unsigned untouched[2];
  unsigned untouched_expected[2];

  source[0] = 0x12345678;
  source[1] = 0xABCDEF01;
  source[2] = 0x00FF00FF;

  dest[0] = 0x77777777;
  dest[1] = 0x77777777;
  dest[2] = 0x77777777;
  dest[3] = 0x77777777;
  dest[4] = 0x77777777;

  expected[0] = 0x77777777;
  expected[1] = 0x12345678;
  expected[2] = 0xABCDEF01;
  expected[3] = 0x00FF00FF;
  expected[4] = 0x77777777;

  untouched[0] = 0x13572468;
  untouched[1] = 0x24681357;

  untouched_expected[0] = 0x13572468;
  untouched_expected[1] = 0x24681357;

  assert((unsigned*)memcpy4(dest + 1, source, 12) == dest + 1,
    "string: memcpy4 should return the destination pointer.\n");
  assert_byte_region((unsigned char*)dest, (unsigned char*)expected, sizeof(dest),
    "string: memcpy4 should copy aligned 4-byte elements exactly and leave surrounding data untouched.\n");

  assert((unsigned*)memcpy4(untouched, source, 0) == untouched,
    "string: memcpy4 should return the destination pointer for zero-length copies.\n");
  assert_byte_region((unsigned char*)untouched, (unsigned char*)untouched_expected, sizeof(untouched),
    "string: memcpy4 should leave the destination unchanged when n is zero.\n");

  say("***memcpy4: ok\n", NULL);
}

// Check memset() raw byte writes and zero-length behavior.
static void check_memset(void) {
  unsigned char buf[6];
  unsigned char expected[6];
  unsigned char untouched[3];
  unsigned char untouched_expected[3];

  buf[0] = 0xAA;
  buf[1] = 0xAA;
  buf[2] = 0xAA;
  buf[3] = 0xAA;
  buf[4] = 0xAA;
  buf[5] = 0xAA;

  expected[0] = 0xAA;
  expected[1] = 0x34;
  expected[2] = 0x34;
  expected[3] = 0x34;
  expected[4] = 0x34;
  expected[5] = 0xAA;

  untouched[0] = 9;
  untouched[1] = 8;
  untouched[2] = 7;

  untouched_expected[0] = 9;
  untouched_expected[1] = 8;
  untouched_expected[2] = 7;

  assert((unsigned char*)memset(buf + 1, 0x1234, 4) == buf + 1,
    "string: memset should return the destination pointer.\n");
  assert_byte_region(buf, expected, 6,
    "string: memset should write the low byte of c across the requested range only.\n");

  assert((unsigned char*)memset(untouched, 0x56, 0) == untouched,
    "string: memset should return the destination pointer for zero-length fills.\n");
  assert_byte_region(untouched, untouched_expected, 3,
    "string: memset should leave the destination unchanged when n is zero.\n");

  say("***memset: ok\n", NULL);
}

// Run the string helper checks one API at a time.
int kernel_main(void) {
  check_strlen();
  check_streq();
  check_strneq();
  check_strncpy();
  check_memcpy();
  check_memcpy2();
  check_memcpy4();
  check_memset();

  say("***string helpers: ok\n", NULL);
  return 0;
}
