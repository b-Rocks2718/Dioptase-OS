#ifndef DEBUG_H
#define DEBUG_H

#include "constants.h"

void panic(char* msg);

void assert(bool condition, char* msg);

#endif // DEBUG_H
