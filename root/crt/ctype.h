#ifndef CTYPE_H
#define CTYPE_H

#include "stdbool.h"

bool isdigit(char c);

// Returns true for ASCII hexadecimal digits used by the compiler front end.
bool isxdigit(char c);

bool isalpha(char c);

bool isalnum(char c);

bool isspace(char c);

bool isprint(char c);

#endif // CTYPE_H
