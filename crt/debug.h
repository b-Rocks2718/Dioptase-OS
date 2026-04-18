#ifndef DEBUG_H
#define DEBUG_H

#include "constants.h"

// Print panic message and halt the system
void panic(char* msg);

// Assert that the given condition is true, and panic with the given message if not
void assert(bool condition, char* msg);

#endif // DEBUG_H
