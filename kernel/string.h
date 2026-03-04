#ifndef STRING_H
#define STRING_H

#include "constants.h"

unsigned strlen(char* str);

bool streq(char* str1, char* str2);

bool strneq(char* str1, char* str2, unsigned n);

#endif // STRING_H