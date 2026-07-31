/*
 * Shared reporting helper for user-space integration tests.
 *
 * The test harness captures lines beginning with "***" and compares them with
 * a golden output file. Successful expectations name the operation and value;
 * failed expectations additionally show the expected value so the raw log is
 * actionable without matching it to a source-line number.
 */

#ifndef USER_TEST_H
#define USER_TEST_H

#include "../root/crt/print.h"

static void user_test_expect_eq(char* operation, int actual, int expected){
  int args[3] = {(int)operation, actual, expected};

  if (actual == expected){
    printf("***PASS %s: got %d expected %d\n", args);
  } else {
    printf("***FAIL %s: got %d expected %d\n", args);
  }
}

#endif // USER_TEST_H
