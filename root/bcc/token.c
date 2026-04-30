#include "../crt/stddef.h"
#include "../crt/stdio.h"
#include "../crt/stdlib.h"

#include "slice.h"
#include "token.h"

void free_token(struct Token* token){
  if (token->type == IDENT) free(token->data.ident_name);
  free(token);
}

void print_token(struct Token token){
  switch (token.type){
    case INT_LIT:
      printf("%d", token.data.int_val);
      break;
    case LONG_LIT:
      printf("%ld", token.data.long_val);
      break;
    case U_INT_LIT:
      printf("%u", token.data.uint_val);
      break;
    case U_LONG_LIT:
      printf("%lu", token.data.ulong_val);
      break;
    case CHAR_LIT:
      printf("%d", (int)token.data.char_val);
      break;
    case STRING_LIT:
      printf("\"%.*s\"", (int)token.data.string_val->len, token.data.string_val->start);
      break;
    case IDENT:
      print_slice(token.data.ident_name);
      break;
    case VOID_TOK:
      printf("void");
      break;
    case INT_TOK:
      printf("int");
      break;
    case RETURN_TOK:
      printf("return");
      break;
    case OPEN_P:
      printf("(");
      break;
    case CLOSE_P:
      printf(")");
      break;
    case OPEN_B:
      printf("{");
      break;
    case CLOSE_B:
      printf("}");
      break;
    case SEMI:
      printf(";");
      break;
    case TILDE:
      printf("~");
      break;
    case INC_TOK:
      printf("++");
      break;
    case DEC_TOK:
      printf("--");
      break;
    case PLUS:
      printf("+");
      break;
    case ASTERISK:
      printf("*");
      break;
    case SLASH:
      printf("/");
      break;
    case PERCENT:
      printf("%%");
      break;
    case MINUS:
      printf("-");
      break;
    case AMPERSAND:
      printf("&");
      break;
    case PIPE:
      printf("|");
      break;
    case CARAT:
      printf("^");
      break;
    case SHIFT_L_TOK:
      printf("<<");
      break;
    case SHIFT_R_TOK:
      printf(">>");
      break;
    case EXCLAMATION:
      printf("!");
      break;
    case DOUBLE_AMPERSAND:
      printf("&&");
      break;
    case DOUBLE_PIPE:
      printf("||");
      break;
    case DOUBLE_EQUALS:
      printf("==");
      break;
    case EQUALS:
      printf("=");
      break;
    case NOT_EQUAL:
      printf("!=");
      break;
    case LESS_THAN:
      printf("<");
      break;
    case GREATER_THAN:
      printf(">");
      break;
    case LESS_THAN_EQ:
      printf("<=");
      break;
    case GREATER_THAN_EQ:
      printf(">=");
      break;
    case PLUS_EQ:
      printf("+=");
      break;
    case MINUS_EQ:
      printf("-=");
      break;
    case TIMES_EQ:
      printf("*=");
      break;
    case DIV_EQ:
      printf("/=");
      break;
    case MOD_EQ:
      printf("%%=");
      break;
    case AND_EQ:
      printf("&=");
      break;
    case OR_EQ:
      printf("|=");
      break;
    case XOR_EQ:
      printf("^=");
      break;
    case SHL_EQ:
      printf("<<=");
      break;
    case SHR_EQ:
      printf(">>=");
      break;
    case IF_TOK:
      printf("if");
      break;
    case ELSE_TOK:
      printf("else");
      break;
    case QUESTION:
      printf("?");
      break;
    case COLON:
      printf(":");
      break;
    case GOTO_TOK:
      printf("goto");
      break;
    case DO_TOK:
      printf("do");
      break;
    case WHILE_TOK:
      printf("while");
      break;
    case FOR_TOK:
      printf("for");
      break;
    case BREAK_TOK:
      printf("break");
      break;
    case CONTINUE_TOK:
      printf("continue");
      break;
    case COMMA:
      printf(",");
      break;
    case STATIC_TOK:
      printf("static");
      break;
    case EXTERN_TOK:
      printf("extern");
      break;
    case SWITCH_TOK:
      printf("switch");
      break;
    case CASE_TOK:
      printf("case");
      break;
    case DEFAULT_TOK:
      printf("default");
      break;
    case UNSIGNED_TOK:
      printf("unsigned");
      break;
    case SIGNED_TOK:
      printf("signed");
      break;
    case LONG_TOK:
      printf("long");
      break;
    case SHORT_TOK:
      printf("short");
      break;
    case CHAR_TOK:
      printf("char");
      break;
    case OPEN_S:
      printf("[");
      break;
    case CLOSE_S:
      printf("]");
      break;
    case SIZEOF_TOK:
      printf("sizeof");
      break;
    case ATTRIBUTE_TOK:
      printf("__attribute__");
      break;
    case STRUCT_TOK:
      printf("struct");
      break;
    case UNION_TOK:
      printf("union");
      break;
    case ENUM_TOK:
      printf("enum");
      break;
    case DOT_TOK:
      printf(".");
      break;
    case ARROW_TOK:
      printf("->");
      break;
    default:
      printf("Token?");
      break;
  };
}