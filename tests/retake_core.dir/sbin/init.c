/* User-mode entry point for the same movement/recording tests run natively.
 * The core modules are linked separately, as they are in the game binary. */
#define RETAKE_GUEST 1
#include "../../../root/retake/tests/test_core.c"
