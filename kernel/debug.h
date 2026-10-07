#ifndef DEBUG_H
#define DEBUG_H

#include "constants.h"

// Print panic message and halt the system
void panic(char* msg);

// Report the source location of a failed assertion, then panic with msg.
// The panic line still contains msg verbatim so .panic baselines that match
// fixed message substrings are unaffected by the extra location line.
void assert_failed(char* file, int line, char* msg);

// Hard assert: always panics when the condition is false, including OS_RELEASE
// builds. Use for corruption and unrecoverable contracts that must remain.
// This is an expression, so it is usable anywhere a void expression is.
#define assert_always(cond, msg) \
  ((cond) ? (void)0 : assert_failed(__FILE__, __LINE__, (msg)))

// Soft assert: panics when the condition is false in non-release builds.
// When OS_RELEASE is defined, neither the condition nor the message is
// evaluated, so the condition must never contain a required side effect.
//
// Test sources (tests/*.c, compiled with -DOS_TEST) always check their soft
// asserts: a test's verdict must not depend on OS_RELEASE, and test asserts
// often wrap the operation under test. Kernel code linked into a test image is
// compiled separately and still follows OS_RELEASE.
#if defined(OS_RELEASE) && !defined(OS_TEST)
#define assert(cond, msg) ((void)0)
#else
#define assert(cond, msg) assert_always(cond, msg)
#endif

#endif // DEBUG_H
