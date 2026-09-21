#include "ctype.h"

// Report whether a character is a decimal digit.
bool isdigit(char c){
  return c >= '0' && c <= '9';
}

// Report whether a character is a hexadecimal digit.
bool isxdigit(char c){
  return isdigit(c) ||
         (c >= 'A' && c <= 'F') ||
         (c >= 'a' && c <= 'f');
}

// Report whether a character is alphabetic.
bool isalpha(char c){
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// Report whether a character is alphanumeric.
bool isalnum(char c){
  return isdigit(c) || isalpha(c);
}

// Report whether a character is whitespace.
bool isspace(char c){
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// Report whether a character is printable ASCII.
bool isprint(char c){
  return c >= ' ' && c <= '~';
}
