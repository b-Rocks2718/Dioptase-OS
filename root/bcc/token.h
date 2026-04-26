#ifndef TOKEN_H
#define TOKEN_H

#include "../crt/stddef.h"

struct Slice;

enum TokenType {
  // tokens with data
  INT_LIT, // contains int
  LONG_LIT, // contains long
  U_INT_LIT, // contains unsigned
  U_LONG_LIT, // contains unsigned long
  CHAR_LIT, // contains char
  STRING_LIT, // contains slice
  IDENT, // contains slice

  // TOKENS WITHOUT DATA
  VOID_TOK,
  INT_TOK,
  RETURN_TOK,
  OPEN_P,
  CLOSE_P,
  OPEN_B,
  CLOSE_B,
  OPEN_S,
  CLOSE_S,
  SEMI,
  TILDE,
  INC_TOK,
  DEC_TOK,
  PLUS,
  ASTERISK,
  SLASH,
  PERCENT,
  MINUS,
  AMPERSAND,
  PIPE,
  CARAT,
  SHIFT_L_TOK,
  SHIFT_R_TOK,
  EXCLAMATION,
  DOUBLE_AMPERSAND,
  DOUBLE_PIPE,
  DOUBLE_EQUALS,
  EQUALS,
  NOT_EQUAL,
  LESS_THAN,
  GREATER_THAN,
  LESS_THAN_EQ,
  GREATER_THAN_EQ,
  PLUS_EQ,
  MINUS_EQ,
  TIMES_EQ,
  DIV_EQ,
  MOD_EQ,
  AND_EQ,
  OR_EQ,
  XOR_EQ,
  SHL_EQ,
  SHR_EQ,
  IF_TOK,
  ELSE_TOK,
  QUESTION,
  COLON,
  GOTO_TOK,
  DO_TOK,
  WHILE_TOK,
  FOR_TOK,
  BREAK_TOK,
  CONTINUE_TOK,
  COMMA,
  STATIC_TOK,
  EXTERN_TOK,
  SWITCH_TOK,
  CASE_TOK,
  DEFAULT_TOK,
  UNSIGNED_TOK,
  SIGNED_TOK,
  LONG_TOK,
  SHORT_TOK,
  CHAR_TOK,
  SIZEOF_TOK,
  ATTRIBUTE_TOK,
  STRUCT_TOK,
  UNION_TOK,
  ENUM_TOK,
  DOT_TOK,
  ARROW_TOK,
};

union TokenVariant {
  int int_val;
  unsigned uint_val;
  long long_val;
  unsigned long ulong_val;
  char char_val;
  struct Slice* string_val;
  struct Slice* ident_name;
};

struct Token {
  enum TokenType type;
  union TokenVariant data;
  char* start;
  size_t len;
};

#endif // TOKEN_H
