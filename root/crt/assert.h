#ifndef ASSERT_H
#define ASSERT_H

#include "stdbool.h"

// Soft assert: panics when the condition is false in non-release builds.
// When OS_RELEASE is defined, this is a no-op function body. bcc has no
// function-like macros, so call-site conditions and message strings are still
// evaluated/emitted. Use for development invariants that may be stripped.
void assert(bool condition, char* msg);

// Hard assert: always panics when the condition is false, including OS_RELEASE
// builds. Use for corruption and unrecoverable contracts that must remain.
void assert_always(bool condition, char* msg);

#endif // ASSERT_H
