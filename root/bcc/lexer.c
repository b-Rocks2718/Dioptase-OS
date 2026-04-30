#include "../crt/stdio.h"
#include "../crt/print.h"
#include "../crt/stdbool.h"
#include "../crt/stdint.h"
#include "../crt/limits.h"
#include "../crt/stdlib.h"
#include "../crt/ctype.h"

#include "slice.h"
#include "token.h"
#include "token_array.h"
#include "lexer.h"
#include "source_location.h"

// Purpose: Tokenize preprocessed source into a stream of tokens.
// Inputs: Uses the global cursor to walk the NUL-terminated source buffer.
// Outputs: Produces Token structures appended to a TokenArray.
// Invariants/Assumptions: Whitespace is skipped; no comments remain (preprocessed).

static char * program;
static char * current;
static char * last_token_start;
static size_t last_token_len;

static void print_error_at(char* ptr, char* message);

// Purpose: Report an unsupported long-only literal spelling in the bootstrap compiler.
// Inputs: start identifies the beginning of the literal for diagnostics.
// Outputs: Emits an actionable lexer error and terminates compilation.
// Invariants/Assumptions: This root/bcc copy intentionally rejects long literals.
static void reject_long_literal(char* start, char* reason) {
  print_error_at(start, reason);
  exit(1);
}

// Purpose: Convert one hexadecimal digit into its numeric value.
// Inputs: c is an ASCII character from source text.
// Outputs: Returns 0-15 for hexadecimal digits, or -1 for any other character.
// Invariants/Assumptions: Only ASCII hex escapes are supported in source code.
static int hex_digit_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

// Purpose: Decode a byte-valued \x hexadecimal escape.
// Inputs: digits points at the first character after \x.
// Outputs: On success, writes the byte value to out_value, the number of consumed
// hex digits to consumed_digits, and returns true.
// Invariants/Assumptions: The Dioptase C compiler models \x escapes as single-byte
// values, so anything above 0xff is rejected instead of silently truncating.
static bool decode_hex_escape(char* digits, size_t* consumed_digits,
                              unsigned char* out_value) {
  unsigned int value = 0;
  size_t len = 0;
  while (true) {
    int digit = hex_digit_value(digits[len]);
    if (digit < 0) break;

    unsigned int next_value = (value << 4) | (unsigned int)digit;
    if (next_value > 0xff) {
      *consumed_digits = len;
      return false;
    }

    value = next_value;
    len += 1;
  }

  *consumed_digits = len;
  if (len == 0) return false;

  *out_value = (unsigned char)value;
  return true;
}

// Purpose: Emit a lexer error diagnostic at an explicit source pointer.
// Inputs: ptr identifies the source byte to report; message is a static string.
// Outputs: Writes an error message and the remaining source line to stderr.
// Invariants/Assumptions: source context has been initialized for ptr.
static void print_error_at(char* ptr, char* message) {
  int args[3];
  int line_args[1];
  struct SourceLocation loc = source_location_from_ptr(ptr);
  char* filename = source_filename_for_ptr(ptr);

  args[0] = (int)filename;
  args[1] = (int)loc.line;
  args[2] = (int)loc.column;
  fdprintf(STDERR, "Lexer error at %s:%zu:%zu: ", args);
  fdputs(STDERR, message);
  fdputs(STDERR, "\n");
  line_args[0] = (int)ptr;
  fdprintf(STDERR, "%s\n", line_args);
}

// Purpose: Emit a lexer error diagnostic for the current cursor.
// Inputs: current points to the byte where tokenization failed.
// Outputs: Writes an error message to stdout.
// Invariants/Assumptions: source context has been initialized for locations.
static void print_error() {
  int args[4];
  int line_args[1];
  struct SourceLocation loc = source_location_from_ptr(current);
  char* filename = source_filename_for_ptr(current);
  if (*current == '\0') {
    args[0] = (int)filename;
    args[1] = (int)loc.line;
    args[2] = (int)loc.column;
    fdprintf(STDERR, "Lexer error at %s:%zu:%zu: unexpected end of input\n",
             args);
    return;
  }
  args[0] = (int)filename;
  args[1] = (int)loc.line;
  args[2] = (int)loc.column;
  args[3] = (int)*current;
  fdprintf(STDERR, "Lexer error at %s:%zu:%zu: unexpected character '%c'\n",
           args);
  line_args[0] = (int)current;
  fdprintf(STDERR, "%s\n", line_args);
}

// Purpose: Measure the byte length of a source span for token bookkeeping.
// Inputs: start/end point into the same source buffer, with end >= start.
// Outputs: Returns the span length in bytes.
// Invariants/Assumptions: The Dioptase ABI defines pointers as 4-byte values in
// docs/abi.md, so the bootstrap compiler can safely materialize the difference
// through unsigned arithmetic even though it rejects direct pointer subtraction.
static size_t span_len(char* start, char* end) {
  return (size_t)((unsigned)end - (unsigned)start);
}

// Purpose: Determine whether the lexer has reached the end of input.
// Inputs: Uses the global cursor and skips trailing whitespace.
// Outputs: Returns true when no more non-space characters remain.
// Invariants/Assumptions: current points into the same buffer as program.
static bool is_at_end() {
  while (isspace((unsigned char)*current)) {
    current += 1;
  }
  if (*current != 0) return false;
  else return true;
}

// Purpose: Skip over ASCII whitespace characters.
// Inputs: Uses the global cursor pointer.
// Outputs: Advances current past whitespace.
// Invariants/Assumptions: Whitespace is not emitted as tokens.
static void skip() {
  while (isspace((unsigned char)*current)) {
    current += 1;
  }
}

// Purpose: Consume a fixed string token at the current cursor.
// Inputs: str is the literal to match.
// Outputs: Returns true on match and updates last_token_* metadata.
// Invariants/Assumptions: Skips leading whitespace before matching.
static bool consume(char* str) {
  skip();
  char* start = current;
  size_t i = 0;
  while (true) {
    char expected = str[i];
    char found = current[i];
    if (expected == 0) {
      /* survived to the end of the expected string */
      current += i;
      last_token_start = start;
      last_token_len = i;
      return true;
    }
    if (expected != found) {
      return false;
    }
    // assertion: found != 0
    i += 1;
  } 
}

// Purpose: Consume a keyword token, enforcing a word boundary.
// Inputs: str is the keyword literal to match.
// Outputs: Returns true on match and updates last_token_* metadata.
// Invariants/Assumptions: Rejects identifiers that merely prefix the keyword.
static bool consume_keyword(char* str) {
  skip();
  char* start = current;
  size_t i = 0;
  while (true) {
    char expected = str[i];
    char found = current[i];
    if (expected == 0) {
      /* survived to the end of the expected string */
      if (!isalnum((unsigned char)found) && found != '_') {
        // word break
        current += i;
        last_token_start = start;
        last_token_len = i;
        return true;
      } else {
        // this is actually an identifier
        return false;
      }
    }
    if (expected != found) {
      return false;
    }
    // assertion: found != 0
    i += 1;
  } 
}

// Purpose: Consume an identifier token and allocate its slice.
// Inputs: token is the destination Token to populate.
// Outputs: Returns true on success and advances current past the identifier.
// Invariants/Assumptions: Identifier slices point into the source buffer.
static bool consume_identifier(struct Token* token) {
  skip();
  if (isalpha((unsigned char)*current) || *current == '_') {
    char * start = current;
    do {
      current += 1;
    } while(isalnum((unsigned char)*current) || *current == '_');

    struct Slice* slice = malloc(sizeof(struct Slice));
    slice->start = start;
    slice->len = span_len(start, current);

    token->type = IDENT;
    token->data.ident_name = slice;
    token->start = start;
    token->len = slice->len;
    return true;
  } else {
    return false;
  }
}

// Purpose: Consume a decimal or hex integer literal token.
// Inputs: token is the destination Token to populate.
// Outputs: Returns true on success and advances current past the literal.
// Invariants/Assumptions: Supports optional u/U and l/L suffixes; accepts .0
// fractional forms as integer literals for integer-only parsing.
static bool consume_literal(struct Token* token) {
  skip();
  if (isdigit((unsigned char)*current)) {
    // number literal
    char * start = current;
    unsigned v = 0;
    bool is_hex = false;
    if (*current == '0' &&
        (current[1] == 'x' || current[1] == 'X') &&
        isxdigit((unsigned char)current[2])) {
      current += 2;
      is_hex = true;
      while (isxdigit((unsigned char)*current)) {
        int digit = hex_digit_value((unsigned char)*current);
        if (v > ((unsigned)UINT_MAX - (unsigned)digit) / 16u) {
          reject_long_literal(start,
                              "bootstrap bcc only supports 32-bit integer literals");
        }
        v = (v * 16u) + (unsigned)digit;
        current += 1;
      }
    } else {
      do {
        unsigned digit = (unsigned)((*current) - '0');
        if (v > ((unsigned)UINT_MAX - digit) / 10u) {
          reject_long_literal(start,
                              "bootstrap bcc only supports 32-bit integer literals");
        }
        v = (10u * v) + digit;
        current += 1;
      } while (isdigit((unsigned char)*current));
    }

    bool saw_u = false;
    bool saw_l = false;
    for (int i = 0; i < 2; i++) {
      if ((*current == 'u' || *current == 'U') && !saw_u) {
        saw_u = true;
        current += 1;
        continue;
      }
      if ((*current == 'l' || *current == 'L') && !saw_l) {
        saw_l = true;
        current += 1;
        continue;
      }
      break;
    }

    if (saw_l) {
      reject_long_literal(start,
                          "bootstrap bcc does not support long integer literals");
    }

    if (saw_u) {
      token->type = U_INT_LIT;
      token->data.uint_val = v;
    } else if (is_hex) {
      if (v <= (unsigned)INT_MAX) {
        token->type = INT_LIT;
        token->data.int_val = (int)v;
      } else {
        token->type = U_INT_LIT;
        token->data.uint_val = v;
      }
    } else {
      if (v <= (unsigned)INT_MAX) {
        token->type = INT_LIT;
        token->data.int_val = (int)v;
      } else {
        reject_long_literal(start,
                            "bootstrap bcc decimal literals above INT_MAX require unsupported long support");
      }
    }
    token->start = start;
    token->len = span_len(start, current);
    return true;
  } else if (*current == '\'') {
    char* start = current;

    // char literal
    consume("\'");

    if (*current == '\0' || *current == '\n') {
      print_error();
      exit(1);
    }

    // detect escape characters
    if (*current == '\\'){
      if (*(current + 1) == 'x') {
        size_t digits = 0;
        unsigned char value = 0;
        if (!decode_hex_escape(current + 2, &digits, &value)) {
          if (digits == 0) {
            print_error_at(current + 1,
                           "expected at least one hexadecimal digit after \\x");
          } else {
            print_error_at(current + 2 + digits,
                           "hex escape exceeds byte value 0xff");
          }
          exit(1);
        }
        token->data.char_val = (char)value;
        current += 2 + digits;
      } else {
        switch (*(current + 1)){
          case '\'':
            token->data.char_val = '\'';
            break;
          case '\"':
            token->data.char_val = '\"';
            break;
          case '\?':
            token->data.char_val = '\?';
            break;
          case '\\':
            token->data.char_val = '\\';
            break;
          case 'a':
            token->data.char_val = '\a';
            break;
          case 'b':
            token->data.char_val = '\b';
            break;
          case 'f':
            token->data.char_val = '\f';
            break;
          case 'n':
            token->data.char_val = '\n';
            break;
          case 'r':
            token->data.char_val = '\r';
            break;
          case 't':
            token->data.char_val = '\t';
            break;
          case 'v':
            token->data.char_val = '\v';
            break;
          case '0':
            token->data.char_val = '\0';
            break;
          default:
            print_error();
            exit(1);
        }

        current += 2;
      }
    } else {
      if (*current == '\'') {
        print_error();
        exit(1);
      }
      token->data.char_val = *current;
      current += 1;
    }

    if (!consume("\'")){
      print_error();
      exit(1);
    }

    token->start = start;
    token->type = CHAR_LIT;
    token->len = span_len(start, current);
    return true;
  } else if (*current == '\"') {
    char* start = current;

    // string literal
    current += 1;
    bool escaped = false;
    while ((*current != '\"' || *(current - 1) == '\\') && *current != '\0') {
      if (*current == '\n') {
        print_error();
        exit(1);
      }
      if (escaped){
        switch (*current){
          case '\'':
          case '\"':
          case '\?':
          case '\\':
          case 'a':
          case 'b':
          case 'f':
          case 'n':
          case 'r':
          case 't':
          case 'v':
          case '0':
            // allowed escapes
            break;
          case 'x':
            if (hex_digit_value(*(current + 1)) < 0) {
              print_error_at(current,
                             "expected at least one hexadecimal digit after \\x");
              exit(1);
            }
            break;
          default:
            // unrecognized escape
            print_error();
            exit(1);
        }
      }

      if (!escaped && *current == '\\') escaped = true;
      else escaped = false;
      current++;
    }

    if (*current != '\"'){
      print_error();
      exit(1);
    }

    current++;

    struct Slice* slice = malloc(sizeof(struct Slice));
    slice->len = span_len(start + 1, current - 1);
    slice->start = start + 1;
    token->data.string_val = slice;
    token->start = start;
    token->type = STRING_LIT;
    token->len = span_len(start, current);
    return true;

  } else {
    return false;
  }
}

// Purpose: Finalize a token that was consumed via a fixed string match.
// Inputs: token is the allocated Token; type is the token type to assign.
// Outputs: Returns token after populating its type/start/len fields.
// Invariants/Assumptions: last_token_* was set by consume/consume_keyword.
static struct Token* finish_simple_token(struct Token* token, enum TokenType type) {
  token->type = type;
  token->start = last_token_start;
  token->len = last_token_len;
  return token;
}

// Purpose: Consume the next available token.
// Inputs: Uses the global cursor to scan the next token.
// Outputs: Returns a heap-allocated Token or NULL if no token matches.
// Invariants/Assumptions: Caller frees the returned token when destroying arrays.
static struct Token* consume_any(){
  struct Token* token = malloc(sizeof(struct Token));

  if (consume_keyword("return")) return finish_simple_token(token, RETURN_TOK);
  if (consume_keyword("void")) return finish_simple_token(token, VOID_TOK);
  if (consume_keyword("if")) return finish_simple_token(token, IF_TOK);
  if (consume_keyword("else")) return finish_simple_token(token, ELSE_TOK);
  if (consume_keyword("do")) return finish_simple_token(token, DO_TOK);
  if (consume_keyword("while")) return finish_simple_token(token, WHILE_TOK);
  if (consume_keyword("for")) return finish_simple_token(token, FOR_TOK);
  if (consume_keyword("goto")) return finish_simple_token(token, GOTO_TOK);
  if (consume_keyword("break")) return finish_simple_token(token, BREAK_TOK);
  if (consume_keyword("continue")) return finish_simple_token(token, CONTINUE_TOK);
  if (consume_keyword("static")) return finish_simple_token(token, STATIC_TOK);
  if (consume_keyword("extern")) return finish_simple_token(token, EXTERN_TOK);
  if (consume_keyword("switch")) return finish_simple_token(token, SWITCH_TOK);
  if (consume_keyword("case")) return finish_simple_token(token, CASE_TOK);
  if (consume_keyword("default")) return finish_simple_token(token, DEFAULT_TOK);
  if (consume_keyword("int")) return finish_simple_token(token, INT_TOK);
  if (consume_keyword("unsigned")) return finish_simple_token(token, UNSIGNED_TOK);
  if (consume_keyword("signed")) return finish_simple_token(token, SIGNED_TOK);
  if (consume_keyword("long")) return finish_simple_token(token, LONG_TOK);
  if (consume_keyword("short")) return finish_simple_token(token, SHORT_TOK);
  if (consume_keyword("char")) return finish_simple_token(token, CHAR_TOK);
  if (consume_keyword("sizeof")) return finish_simple_token(token, SIZEOF_TOK);
  if (consume_keyword("__attribute__")) return finish_simple_token(token, ATTRIBUTE_TOK);
  if (consume_keyword("struct")) return finish_simple_token(token, STRUCT_TOK);
  if (consume_keyword("union")) return finish_simple_token(token, UNION_TOK);
  if (consume_keyword("enum")) return finish_simple_token(token, ENUM_TOK);

  if (consume(".")) return finish_simple_token(token, DOT_TOK);
  if (consume("->")) return finish_simple_token(token, ARROW_TOK);
  if (consume(",")) return finish_simple_token(token, COMMA);
  if (consume("?")) return finish_simple_token(token, QUESTION);
  if (consume(":")) return finish_simple_token(token, COLON);
  if (consume(";")) return finish_simple_token(token, SEMI);
  if (consume("(")) return finish_simple_token(token, OPEN_P);
  if (consume(")")) return finish_simple_token(token, CLOSE_P);
  if (consume("{")) return finish_simple_token(token, OPEN_B);
  if (consume("}")) return finish_simple_token(token, CLOSE_B);
  if (consume("[")) return finish_simple_token(token, OPEN_S);
  if (consume("]")) return finish_simple_token(token, CLOSE_S);
  if (consume("~")) return finish_simple_token(token, TILDE);
  if (consume("++")) return finish_simple_token(token, INC_TOK);
  if (consume("--")) return finish_simple_token(token, DEC_TOK);
  if (consume("+=")) return finish_simple_token(token, PLUS_EQ);
  if (consume("-=")) return finish_simple_token(token, MINUS_EQ);
  if (consume("*=")) return finish_simple_token(token, TIMES_EQ);
  if (consume("/=")) return finish_simple_token(token, DIV_EQ);
  if (consume("%=")) return finish_simple_token(token, MOD_EQ);
  if (consume("+")) return finish_simple_token(token, PLUS);
  if (consume("-")) return finish_simple_token(token, MINUS);
  if (consume("*")) return finish_simple_token(token, ASTERISK);
  if (consume("/")) return finish_simple_token(token, SLASH);
  if (consume("%")) return finish_simple_token(token, PERCENT);
  if (consume("&&")) return finish_simple_token(token, DOUBLE_AMPERSAND);
  if (consume("||")) return finish_simple_token(token, DOUBLE_PIPE);
  if (consume("&=")) return finish_simple_token(token, AND_EQ);
  if (consume("|=")) return finish_simple_token(token, OR_EQ);
  if (consume("^=")) return finish_simple_token(token, XOR_EQ);
  if (consume(">>=")) return finish_simple_token(token, SHR_EQ);
  if (consume("<<=")) return finish_simple_token(token, SHL_EQ);
  if (consume("&")) return finish_simple_token(token, AMPERSAND);
  if (consume("|")) return finish_simple_token(token, PIPE);
  if (consume("^")) return finish_simple_token(token, CARAT);
  if (consume(">>")) return finish_simple_token(token, SHIFT_R_TOK);
  if (consume("<<")) return finish_simple_token(token, SHIFT_L_TOK);
  if (consume("!=")) return finish_simple_token(token, NOT_EQUAL);
  if (consume("!")) return finish_simple_token(token, EXCLAMATION);
  if (consume("==")) return finish_simple_token(token, DOUBLE_EQUALS);
  if (consume(">=")) return finish_simple_token(token, GREATER_THAN_EQ);
  if (consume("<=")) return finish_simple_token(token, LESS_THAN_EQ);
  if (consume("=")) return finish_simple_token(token, EQUALS);
  if (consume(">")) return finish_simple_token(token, GREATER_THAN);
  if (consume("<")) return finish_simple_token(token, LESS_THAN);

  if (consume_identifier(token)) return token;
  if (consume_literal(token)) return token;

  free(token);
  return NULL;
}

// Purpose: Tokenize a preprocessed source buffer into a TokenArray.
// Inputs: prog is the NUL-terminated source buffer to lex.
// Outputs: Returns a TokenArray or NULL on error.
// Invariants/Assumptions: prog remains valid for the lifetime of token slices.
struct TokenArray* lex(char* prog){
  program = prog;
  current = prog;

  struct TokenArray* result = create_token_array(1000);

  struct Token* current_token = consume_any();
  while (current_token != NULL){    
    token_array_append(result, current_token);
    current_token = consume_any();
  }

  if (!is_at_end()) {
    destroy_token_array(result);
    print_error();
    return NULL;
  }

  return result;
}
