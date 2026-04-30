#include "ctype.h"

bool isdigit(char c){
  return c >= '0' && c <= '9';
}

bool isxdigit(char c){
  return isdigit(c) ||
         (c >= 'A' && c <= 'F') ||
         (c >= 'a' && c <= 'f');
}

bool isalpha(char c){
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool isalnum(char c){
  return isdigit(c) || isalpha(c);
}

bool isspace(char c){
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

bool isprint(char c){
  return c >= ' ' && c <= '~';
}
