#include "string.h"

// length of a NUL-terminated string, not counting the NUL
unsigned strlen(char* str){
  unsigned len = 0;
  while (str[len] != '\0'){
    len++;
  }
  return len;
}

// compare two NUL-terminated strings for equality
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

// compare the first n bytes of two strings for equality
// returns true if the first n bytes are the same
// or if both strings end before n and are the same up to that point
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

// Copies up to `n` bytes from `src`. If `src` ends earlier, this helper writes
// exactly one trailing NUL and leaves the rest of `dest` unchanged.
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
