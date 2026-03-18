#include "string.h"

unsigned strlen(char* str){
  unsigned len = 0;
  while (str[len] != '\0'){
    len++;
  }
  return len;
}

bool streq(char* str1, char* str2){
  unsigned i = 0;
  while (str1[i] != '\0' && str2[i] != '\0'){
    if (str1[i] != str2[i]){
      return false;
    }
    i++;
  }
  return str1[i] == '\0' && str2[i] == '\0';
}

bool strneq(char* str1, char* str2, unsigned n){
  unsigned i = 0;
  while (i < n && str1[i] != '\0' && str2[i] != '\0'){
    if (str1[i] != str2[i]){
      return false;
    }
    i++;
  }
  return i == n || (str1[i] == '\0' && str2[i] == '\0');
}

// Copies at most `n` bytes and only writes a single trailing NUL when the
// source string ends early. This is the behavior current kernel callers rely
// on, even though it is weaker than ISO C `strncpy` zero-padding semantics.
char* strncpy(char* dest, char* src, unsigned n){
  unsigned i = 0;
  while (i < n && src[i] != '\0'){
    dest[i] = src[i];
    i++;
  }
  if (i < n){
    dest[i] = '\0';
  }
  return dest;
}
