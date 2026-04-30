#include "../crt/stdbool.h"
#include "../crt/stdlib.h"
#include "../crt/stdio.h"
#include "../crt/print.h"

#include "AST.h"
#include "arena.h"
#include "parser.h"
#include "token.h"
#include "token_array.h"
#include "source_location.h"
#include "slice.h"

static struct Token * program;
static size_t prog_size;
static struct Token * current;
// Track the furthest consumed token so backtracking reports a useful error location.
static size_t max_consumed_index;
static bool max_consumed_valid;

// Purpose: Convert one token pointer into its index within the token array.
// Inputs: token points into the same contiguous array as program.
// Outputs: Returns the zero-based token index.
// Invariants/Assumptions: docs/abi.md defines pointers as 4-byte values, so
// the bootstrap compiler can recover indices through unsigned byte arithmetic
// even though it rejects direct typed-pointer subtraction.
static size_t token_index(struct Token* token) {
  return ((size_t)((unsigned)token - (unsigned)program)) / sizeof(struct Token);
}

// Purpose: Record the furthest token consumed for error reporting.
// Inputs: token is the most recently consumed token.
// Outputs: Updates max_consumed_index/max_consumed_valid.
// Invariants/Assumptions: token points into the program token array.
static void record_consumed_token(struct Token* token) {
  size_t index = token_index(token);
  if (!max_consumed_valid || index > max_consumed_index) {
    max_consumed_index = index;
    max_consumed_valid = true;
  }
}

// Purpose: Compute a pointer into the source text for error reporting.
// Inputs: Uses parser state to select the most informative location.
// Outputs: Returns a pointer into the source text or source_text_end().
// Invariants/Assumptions: source context has been initialized before parsing.
static char* parser_error_ptr(void) {
  if (max_consumed_valid) {
    if (max_consumed_index < prog_size) {
      return program[max_consumed_index].start;
    }
    return source_text_end();
  }
  if (token_index(current) < prog_size) {
    return current->start;
  }
  return source_text_end();
}

// Purpose: Emit a formatted parse error at a source pointer.
// Inputs: ptr locates the error; message is a fixed diagnostic string.
// Outputs: Writes an error message to stdout.
// Invariants/Assumptions: ptr is within the current source buffer.
static void parse_error_at(char* ptr, char* message) {
  int args[4];
  struct SourceLocation loc = source_location_from_ptr(ptr);
  char* filename = source_filename_for_ptr(ptr);

  args[0] = (int)filename;
  args[1] = (int)loc.line;
  args[2] = (int)loc.column;
  args[3] = (int)message;
  fdprintf(STDOUT, "Parse error at %s:%zu:%zu: %s\n", args);
}

// Purpose: Emit a generic parse error using the current parser location.
// Inputs: Uses internal parser state to locate the failure.
// Outputs: Writes a diagnostic message to stdout.
// Invariants/Assumptions: parser_error_ptr handles end-of-input safely.
static void print_error() {
  parse_error_at(parser_error_ptr(), "unexpected token");
}

// Purpose: Convert one hexadecimal digit into its numeric value.
// Inputs: c is an ASCII character from source text.
// Outputs: Returns 0-15 for hexadecimal digits, or -1 for any other character.
// Invariants/Assumptions: String and character escapes are parsed from raw source bytes.
static int hex_digit_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

// Purpose: Decode a byte-valued \x hexadecimal escape from raw source text.
// Inputs: digits points at the first character after \x in the source buffer.
// Outputs: On success, writes the byte value to out_value, the number of consumed
// hex digits to consumed_digits, and returns true.
// Invariants/Assumptions: The compiler treats \x escapes in strings as single-byte
// values and rejects anything above 0xff rather than truncating.
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

// Purpose: Allocate a new expression node in the arena.
// Inputs: type is the expression kind; loc is the source location pointer.
// Outputs: Returns an initialized Expr with no type assigned yet.
// Invariants/Assumptions: loc points into the preprocessed source buffer.
static struct Expr* alloc_expr(enum ExprType type, char* loc) {
  struct Expr* expr = arena_alloc(sizeof(struct Expr));
  expr->loc = loc;
  expr->value_type = NULL;
  expr->type = type;
  return expr;
}

// Purpose: Allocate a new statement node in the arena.
// Inputs: type is the statement kind; loc is the source location pointer.
// Outputs: Returns an initialized Statement node.
// Invariants/Assumptions: loc points into the preprocessed source buffer.
static struct Statement* alloc_stmt(enum StatementType type, char* loc) {
  struct Statement* stmt = arena_alloc(sizeof(struct Statement));
  stmt->loc = loc;
  stmt->type = type;
  return stmt;
}

// Purpose: Consume a token of the expected type.
// Inputs: expected is the token type to match.
// Outputs: Returns true on match and advances the cursor.
// Invariants/Assumptions: current points into the program array.
static bool consume(enum TokenType expected) {
  if (token_index(current) < prog_size && expected == current->type) {
    current++;
    record_consumed_token(current - 1);
    return true;
  } else {
    return false;
  }
}

// Purpose: Consume a token with associated data and return its variant.
// Inputs: expected is the token type to match.
// Outputs: Returns a pointer to the token variant on success, else NULL.
// Invariants/Assumptions: current points into the program array.
static union TokenVariant* consume_with_data(enum TokenType expected) {
  if (token_index(current) < prog_size && expected == current->type) {
    current++;
    record_consumed_token(current - 1);
    return &((current - 1)->data);
  } else {
    return NULL;
  }
}

// Purpose: Consume a unary operator token and map it to an UnOp.
// Inputs: Uses the current token stream position.
// Outputs: Returns the matching UnOp or 0 if no unary operator matches.
// Invariants/Assumptions: Tokens are produced by the lexer without comments.
static enum UnOp consume_unary_op(){
  if (consume(TILDE)) return COMPLEMENT;
  if (consume(MINUS)) return NEGATE;
  if (consume(EXCLAMATION)) return BOOL_NOT;
  if (consume(PLUS)) return UNARY_PLUS;
  return 0;
}

// Purpose: Consume a binary operator token and map it to a BinOp.
// Inputs: Uses the current token stream position.
// Outputs: Returns the matching BinOp or 0 if no binary operator matches.
// Invariants/Assumptions: Token precedence is handled by the Pratt parser.
static enum BinOp consume_binary_op(){
  if (consume(PLUS)) return ADD_OP;
  if (consume(MINUS)) return SUB_OP;
  if (consume(ASTERISK)) return MUL_OP;
  if (consume(SLASH)) return DIV_OP;
  if (consume(PERCENT)) return MOD_OP;
  if (consume(AMPERSAND)) return BIT_AND;
  if (consume(PIPE)) return BIT_OR;
  if (consume(CARAT)) return BIT_XOR;
  if (consume(SHIFT_L_TOK)) return BIT_SHL;
  if (consume(SHIFT_R_TOK)) return BIT_SHR;
  if (consume(DOUBLE_AMPERSAND)) return BOOL_AND;
  if (consume(DOUBLE_PIPE)) return BOOL_OR;
  if (consume(DOUBLE_EQUALS)) return BOOL_EQ;
  if (consume(NOT_EQUAL)) return BOOL_NEQ;
  if (consume(LESS_THAN)) return BOOL_LE;
  if (consume(LESS_THAN_EQ)) return BOOL_LEQ;
  if (consume(GREATER_THAN)) return BOOL_GE;
  if (consume(GREATER_THAN_EQ)) return BOOL_GEQ;
  if (consume(EQUALS)) return ASSIGN_OP;
  if (consume(PLUS_EQ)) return PLUS_EQ_OP;
  if (consume(MINUS_EQ)) return MINUS_EQ_OP;
  if (consume(TIMES_EQ)) return MUL_EQ_OP;
  if (consume(DIV_EQ)) return DIV_EQ_OP;
  if (consume(MOD_EQ)) return MOD_EQ_OP;
  if (consume(AND_EQ)) return AND_EQ_OP;
  if (consume(OR_EQ)) return OR_EQ_OP;
  if (consume(XOR_EQ)) return XOR_EQ_OP;
  if (consume(SHL_EQ)) return SHL_EQ_OP;
  if (consume(SHR_EQ)) return SHR_EQ_OP;
  if (consume(QUESTION)) return TERNARY_OP;
  if (consume(COMMA)) return COMMA_OP;
  return 0;
}

// Purpose: Parse a parenthesized variable expression for ++/-- handling.
// Inputs: Consumes tokens from the current cursor.
// Outputs: Returns a VAR expression (possibly wrapped in parentheses) or NULL.
// Invariants/Assumptions: Only identifiers and parentheses are accepted here.
struct Expr* parse_paren_var(){
  struct Expr* expr;
  if ((expr = parse_var())) return expr;
  else if (consume(OPEN_P)){
    struct Token* old_current = current - 1;
    struct Expr* inner = parse_paren_var();
    if (inner == NULL) {
      current = old_current;
      return NULL;
    } else if (!consume(CLOSE_P)){
      current = old_current;
      return NULL;
    }
    return inner;
  } else return NULL;
}

// Purpose: Parse prefix ++/-- by translating to compound assignments.
// Inputs: Consumes tokens from the current cursor.
// Outputs: Returns an expression node or NULL if no prefix op matches.
// Invariants/Assumptions: Operand parsing is deferred to parse_factor.
struct Expr* parse_pre_op(){
  if (consume(INC_TOK)){
    char* op_loc = (current - 1)->start;
    struct Expr* inner = parse_factor();
    if (inner == NULL) {
      current--;
      return NULL;
    }
    union ConstVariant const_data;
    const_data.int_val = 1;
    struct LitExpr one = {INT_CONST, const_data};
    struct Expr* lit_expr = alloc_expr(LIT, op_loc);
    lit_expr->expr.lit_expr = one;

    struct BinaryExpr add_one = {PLUS_EQ_OP, inner, lit_expr};

    struct Expr* result = alloc_expr(BINARY, op_loc);
    result->expr.bin_expr = add_one;
    return result;
  } else if (consume(DEC_TOK)){
    char* op_loc = (current - 1)->start;
    struct Expr* inner = parse_factor();
    if (inner == NULL) {
      current--;
      return NULL;
    }
    union ConstVariant const_data;
    const_data.int_val = 1;
    struct LitExpr one = {INT_CONST, const_data};
    struct Expr* lit_expr = alloc_expr(LIT, op_loc);
    lit_expr->expr.lit_expr = one;

    struct BinaryExpr sub_one = {MINUS_EQ_OP, inner, lit_expr};

    struct Expr* result = alloc_expr(BINARY, op_loc);
    result->expr.bin_expr = sub_one;
    return result;
  } else {
    return NULL;
  }
}

// Purpose: Parse an identifier expression.
// Inputs: Consumes IDENT tokens from the current cursor.
// Outputs: Returns a VAR expression or NULL if no identifier matches.
// Invariants/Assumptions: The identifier slice refers into the token stream.
struct Expr* parse_var(){
  union TokenVariant* data;
  if ((data = consume_with_data(IDENT))){
    char* name_loc = (current - 1)->start;
    struct VarExpr var_expr = { data->ident_name };

    struct Expr* expr = alloc_expr(VAR, name_loc);
    expr->expr.var_expr = var_expr;
    return expr;
  } else return NULL;
}

// Purpose: Parse a single type specifier token.
// Inputs: Consumes type specifier tokens from the current cursor.
// Outputs: Returns the parsed TypeSpecifier or 0 if none matched.
// Invariants/Assumptions: Caller handles combinations of specifiers.
struct TypeSpecifier parse_type_spec(){
  if (consume(INT_TOK)) {
    struct TypeSpecifier spec = { INT_SPEC, NULL };
    return spec;
  }
  if (consume(SIGNED_TOK)) {
    struct TypeSpecifier spec = { SIGNED_SPEC, NULL };
    return spec;
  }
  if (consume(UNSIGNED_TOK)) {
    struct TypeSpecifier spec = { UNSIGNED_SPEC, NULL };
    return spec;
  }
  if (consume(LONG_TOK)) {
    struct TypeSpecifier spec = { LONG_SPEC, NULL };
    return spec;
  }
  if (consume(SHORT_TOK)) {
    struct TypeSpecifier spec = { SHORT_SPEC, NULL };
    return spec;
  }
  if (consume(CHAR_TOK)) {
    struct TypeSpecifier spec = { CHAR_SPEC, NULL };
    return spec;
  }
  if (consume(VOID_TOK)) {
    struct TypeSpecifier spec = { VOID_SPEC, NULL };
    return spec;
  }
  if (consume(STRUCT_TOK)){
    union TokenVariant* data = consume_with_data(IDENT);
    if (data != NULL) {
      struct TypeSpecifier spec = { STRUCT_SPEC, data->ident_name };
      return spec;
    } else {
      parse_error_at(parser_error_ptr(), "expected identifier after 'struct'");
      struct TypeSpecifier spec = { -1, NULL };
      return spec;
    }
  }
  if (consume(UNION_TOK)){
    union TokenVariant* data = consume_with_data(IDENT);
    if (data != NULL) {
      struct TypeSpecifier spec = { UNION_SPEC, data->ident_name };
      return spec;
    } else {
      parse_error_at(parser_error_ptr(), "expected identifier after 'union'");
      struct TypeSpecifier spec = { -1, NULL };
      return spec;
    }
  }
  if (consume(ENUM_TOK)){
    union TokenVariant* data = consume_with_data(IDENT);
    if (data != NULL) {
      struct TypeSpecifier spec = { ENUM_SPEC, data->ident_name };
      return spec;
    } else {
      parse_error_at(parser_error_ptr(), "expected identifier after 'enum'");
      struct TypeSpecifier spec = { -1, NULL };
      return spec;
    }
  }
  else {
    struct TypeSpecifier spec = { -1, NULL };
    return spec;
  }
}

// Purpose: Parse a list of type specifiers.
// Inputs: Consumes a sequence of type specifier tokens.
// Outputs: Returns a linked list of TypeSpecList nodes or NULL.
// Invariants/Assumptions: Caller validates duplicates and compatibility.
struct TypeSpecList* parse_type_specs(){
  struct TypeSpecifier spec = parse_type_spec();
  if (spec.type == -1) return NULL;
  struct TypeSpecList* specs = arena_alloc(sizeof(struct TypeSpecList));
  specs->spec = spec;
  specs->next = parse_type_specs();
  return specs;
}

// Purpose: Test whether a type specifier list contains a specific specifier.
// Inputs: types is the list; spec is the specifier to search for.
// Outputs: Returns true if spec is present.
// Invariants/Assumptions: types is a well-formed linked list.
struct TypeSpecifier* spec_list_contains(struct TypeSpecList* types, enum TypeSpecifierType spec){
  if (types->spec.type == spec) return &types->spec;
  else if (types->next == NULL) return NULL;
  else return spec_list_contains(types->next, spec);
}

// Purpose: Detect duplicate or contradictory type specifiers in a list.
// Inputs: types is the list of parsed specifiers.
// Outputs: Returns true if the specifier list is valid.
// Invariants/Assumptions: types is a well-formed linked list.
bool spec_list_valid(struct TypeSpecList* types){
  unsigned num_ints = 0;
  unsigned num_unsigneds = 0;
  unsigned num_signeds = 0;
  unsigned num_longs = 0;
  unsigned num_shorts = 0;
  unsigned num_chars = 0;
  unsigned num_voids = 0;
  unsigned num_structs = 0;
  unsigned num_unions = 0;
  unsigned num_enums = 0;
  struct TypeSpecList* cur = types;
  while (cur != NULL){
    switch (cur->spec.type){
      case INT_SPEC:
        num_ints++;
        break;
      case UNSIGNED_SPEC:
        num_unsigneds++;
        break;
      case SIGNED_SPEC:
        num_signeds++;
        break;
      case LONG_SPEC:
        num_longs++;
        break;
      case SHORT_SPEC:
        num_shorts++;
        break;
      case CHAR_SPEC:
        num_chars++;
        break;
      case VOID_SPEC:
        num_voids++;
        break;
      case STRUCT_SPEC:
        num_structs++;
        break;
      case UNION_SPEC:
        num_unions++;
        break;
      case ENUM_SPEC:
        num_enums++;
        break;
      default:
        break;
    }
    cur = cur->next;
  }
  int int_kind_sum = num_ints + num_signeds + 
      num_unsigneds + num_longs + 
      num_shorts + num_chars;

  if (num_ints > 1) return false;
  if (num_unsigneds > 1) return false;
  if (num_signeds > 1) return false;
  if (num_longs > 1) return false;
  if (num_shorts > 1) return false;
  if (num_chars > 1) return false;
  if (num_voids > 1) return false;
  if (num_structs > 1) return false;
  if (num_unions > 1) return false;
  if (num_enums > 1) return false;
  if (num_voids > 0 && int_kind_sum > 0) 
    return false;

  if (num_signeds + num_unsigneds > 1) return false;
  if (num_chars + num_shorts + num_longs > 1) return false;
  if (num_chars + num_ints > 1) return false;

  if (num_structs + num_unions + num_enums > 1) return false;

  if (num_structs > 0 && (num_voids + int_kind_sum) > 0) return false;
  if (num_unions > 0 && (num_voids + int_kind_sum) > 0) return false;
  if (num_enums > 0 && (num_voids + int_kind_sum) > 0) return false;

  return true;
}

struct Type* type_spec_to_type(struct TypeSpecList* types){
  struct TypeSpecifier* found = NULL;
  if (types == NULL) return NULL;
  else if (!spec_list_valid(types)) {
    parse_error_at(parser_error_ptr(), "invalid type specifiers");
    return NULL;
  } else if (spec_list_contains(types, LONG_SPEC)) {
    parse_error_at(parser_error_ptr(),
                   "bootstrap bcc does not support long type specifiers");
    return NULL;
  } if (spec_list_contains(types, UNSIGNED_SPEC) && spec_list_contains(types, SHORT_SPEC)){
    struct Type* type = arena_alloc(sizeof(struct Type));
    type->type = USHORT_TYPE;
    return type;
  } else if (spec_list_contains(types, SIGNED_SPEC) && spec_list_contains(types, SHORT_SPEC)){
    struct Type* type = arena_alloc(sizeof(struct Type));
    type->type = SHORT_TYPE;
    return type;
  } else if (spec_list_contains(types, UNSIGNED_SPEC) && spec_list_contains(types, LONG_SPEC)){
    struct Type* type = arena_alloc(sizeof(struct Type));
    type->type = ULONG_TYPE;
    return type;
  } else if (spec_list_contains(types, SIGNED_SPEC) && spec_list_contains(types, LONG_SPEC)){
    struct Type* type = arena_alloc(sizeof(struct Type));
    type->type = LONG_TYPE;
    return type;
  } else if (spec_list_contains(types, SIGNED_SPEC) && spec_list_contains(types, CHAR_SPEC)){
    struct Type* type = arena_alloc(sizeof(struct Type));
    type->type = SCHAR_TYPE;
    return type;
  } else if (spec_list_contains(types, UNSIGNED_SPEC) && spec_list_contains(types, CHAR_SPEC)){
    struct Type* type = arena_alloc(sizeof(struct Type));
    type->type = UCHAR_TYPE;
    return type;
  } else if (spec_list_contains(types, CHAR_SPEC)){
    struct Type* type = arena_alloc(sizeof(struct Type));
    type->type = CHAR_TYPE;
    return type;
  } else if (spec_list_contains(types, SHORT_SPEC)){
    struct Type* type = arena_alloc(sizeof(struct Type));
    type->type = SHORT_TYPE;
    return type;
  } else if (spec_list_contains(types, LONG_SPEC)){
    struct Type* type = arena_alloc(sizeof(struct Type));
    type->type = LONG_TYPE;
    return type;
  } else if (spec_list_contains(types, VOID_SPEC)){
    struct Type* type = arena_alloc(sizeof(struct Type));
    type->type = VOID_TYPE;
    return type;
  } else if ((found = spec_list_contains(types, STRUCT_SPEC)) != NULL){
    struct Type* type = arena_alloc(sizeof(struct Type));
    type->type = STRUCT_TYPE;
    struct StructType struct_data;
    struct_data.name = found->name;
    type->type_data.struct_type = struct_data;
    return type;
  } else if ((found = spec_list_contains(types, UNION_SPEC)) != NULL){
    struct Type* type = arena_alloc(sizeof(struct Type));
    type->type = UNION_TYPE;
    struct UnionType union_data;
    union_data.name = found->name;
    type->type_data.union_type = union_data;
    return type;
  } else if ((found = spec_list_contains(types, ENUM_SPEC)) != NULL){
    struct Type* type = arena_alloc(sizeof(struct Type));
    type->type = ENUM_TYPE;
    struct EnumType enum_data;
    enum_data.name = found->name;
    type->type_data.enum_type = enum_data;
    return type;
  }
  // at this point it must be an int type 
  else if (spec_list_contains(types, UNSIGNED_SPEC)){
    struct Type* type = arena_alloc(sizeof(struct Type));
    type->type = UINT_TYPE;
    return type;
  } else {
    struct Type* type = arena_alloc(sizeof(struct Type));
    type->type = INT_TYPE;
    return type;
  }
}

// Purpose: Parse a parameter type (base specifiers plus abstract declarator).
// Inputs: Consumes tokens from the current cursor.
// Outputs: Returns a Type or NULL on failure.
// Invariants/Assumptions: Handles pointer declarators via parse_abstract_declarator.
struct Type* parse_param_type(){
  struct TypeSpecList* types = parse_type_specs();
  return type_spec_to_type(types);
}

// Purpose: Parse an integer literal token usable as an array bound.
// Inputs: Consumes a single integer literal token.
// Outputs: Returns true and writes the size on success; false otherwise.
// Invariants/Assumptions: Only integer literal tokens are accepted.
static bool parse_array_size_literal(size_t* size_out){
  union TokenVariant* data = consume_with_data(INT_LIT);
  if (data != NULL){
    *size_out = (size_t)data->int_val;
    return true;
  }
  data = consume_with_data(U_INT_LIT);
  if (data != NULL){
    *size_out = (size_t)data->uint_val;
    return true;
  }
  return false;
}

// Purpose: Convert an abstract declarator into a concrete type.
// Inputs: declarator is the parsed abstract declarator; base_type is the core type.
// Outputs: Returns the derived type or NULL on failure.
struct Type* process_abstract_declarator(
    struct AbstractDeclarator* declarator,
    struct Type* base_type);

// Purpose: Parse parameters for an abstract function declarator.
// Inputs: Consumes tokens after the opening '('.
// Outputs: Returns true on success and writes the param list (NULL for empty).
// Invariants/Assumptions: Caller already consumed the opening '(' token.
static bool parse_abstract_params_after_open(struct ParamTypeList** params_out,
                                             struct Token* rewind_to){
  struct Token* before_void = current;
  if (consume(VOID_TOK)){
    if (consume(CLOSE_P)){
      *params_out = NULL;
      return true;
    }
    current = before_void;
  }
  if (consume(CLOSE_P)){
    *params_out = NULL;
    return true;
  }

  struct ParamTypeList* head = NULL;
  struct ParamTypeList** tail = &head;
  while (true){
    struct Type* base_type = parse_param_type();
    if (base_type == NULL){
      current = rewind_to;
      return false;
    }
    struct AbstractDeclarator* declarator = parse_abstract_declarator();
    if (declarator == NULL){
      current = rewind_to;
      return false;
    }
    struct Type* param_type = process_abstract_declarator(declarator, base_type);
    struct ParamTypeList* node = arena_alloc(sizeof(struct ParamTypeList));
    node->type = param_type;
    node->next = NULL;
    *tail = node;
    tail = &node->next;
    if (consume(COMMA)) continue;
    if (consume(CLOSE_P)) break;
    current = rewind_to;
    return false;
  }
  *params_out = head;
  return true;
}

// Purpose: Parse a direct abstract declarator (base or parenthesized).
// Inputs: Consumes tokens from the current cursor.
// Outputs: Returns an AbstractDeclarator or NULL if absent.
// Invariants/Assumptions: Only pointer/identifier-free declarators are allowed.
struct AbstractDeclarator* parse_direct_abstract_declarator(){
  struct Token* old_current = current;
  struct AbstractDeclarator* declarator = NULL;

  if (consume(OPEN_P)){
    declarator = parse_abstract_declarator();
    if (declarator == NULL || !consume(CLOSE_P)){
      current = old_current;
      declarator = NULL;
    }
  }

  if (declarator == NULL){
    declarator = arena_alloc(sizeof(struct AbstractDeclarator));
    declarator->type = ABSTRACT_BASE;
  }

  while (true){
    if (consume(OPEN_S)){
      size_t size = 0;
      if (!parse_array_size_literal(&size) || !consume(CLOSE_S)){
        current = old_current;
        return NULL;
      }
      struct AbstractDeclarator* array_decl = arena_alloc(sizeof(struct AbstractDeclarator));
      array_decl->type = ABSTRACT_ARRAY;
      struct AbstractArray* array_data = arena_alloc(sizeof(struct AbstractArray));
      array_data->next = declarator;
      array_data->size = size;
      array_decl->data.array_type = array_data;
      declarator = array_decl;
    } else if (consume(OPEN_P)){
      struct ParamTypeList* params = NULL;
      if (!parse_abstract_params_after_open(&params, old_current)){
        return NULL;
      }
      struct AbstractDeclarator* fun_decl = arena_alloc(sizeof(struct AbstractDeclarator));
      fun_decl->type = ABSTRACT_FUNCTION;
      struct AbstractFunction* fun_data = arena_alloc(sizeof(struct AbstractFunction));
      fun_data->next = declarator;
      fun_data->params = params;
      fun_decl->data.function_type = fun_data;
      declarator = fun_decl;
    } else {
      break;
    }
  }

  return declarator;
}

// Purpose: Parse an abstract declarator (pointer chains).
// Inputs: Consumes tokens from the current cursor.
// Outputs: Returns an AbstractDeclarator or NULL if absent.
// Invariants/Assumptions: Used for casts and parameter type parsing.
struct AbstractDeclarator* parse_abstract_declarator(){
  struct Token* old_current = current;
  if (consume(ASTERISK)){
    struct AbstractDeclarator* declarator = parse_abstract_declarator();
    if (declarator == NULL){
      current = old_current;
      return NULL;
    }
    struct AbstractDeclarator* result = arena_alloc(sizeof(struct AbstractDeclarator));
    result->type = ABSTRACT_POINTER;
    struct AbstractPointer* pointer_data = arena_alloc(sizeof(struct AbstractPointer));
    pointer_data->next = declarator;
    result->data.pointer_type = pointer_data;
    return result;
  }
  struct AbstractDeclarator* declarator = parse_direct_abstract_declarator();
  if (declarator != NULL){
    return declarator;
  } else {
    struct AbstractDeclarator* declarator = arena_alloc(sizeof(struct AbstractDeclarator));
    declarator->type = ABSTRACT_BASE;
    return declarator;
  }
}

struct Type* process_abstract_declarator(
    struct AbstractDeclarator* declarator, 
    struct Type* base_type){
  struct Type* result;
  switch (declarator->type){
    case ABSTRACT_BASE:
      result = base_type;
      break;
    case ABSTRACT_POINTER: {
      struct Type* ptr_type = arena_alloc(sizeof(struct Type));
      ptr_type->type = POINTER_TYPE;
      ptr_type->type_data.pointer_type.referenced_type = base_type;
      result = process_abstract_declarator(declarator->data.pointer_type->next, ptr_type);
      break;
    }
    case ABSTRACT_ARRAY: {
      struct Type* arr_type = arena_alloc(sizeof(struct Type));
      arr_type->type = ARRAY_TYPE;
      arr_type->type_data.array_type.size = declarator->data.array_type->size;
      arr_type->type_data.array_type.element_type = base_type;
      result = process_abstract_declarator(declarator->data.array_type->next, arr_type);
      break;
    }
    case ABSTRACT_FUNCTION:
      if (base_type->type == FUN_TYPE) {
        parse_error_at(parser_error_ptr(), "function cannot return function type");
        return NULL;
      }
      struct Type* fun_type = arena_alloc(sizeof(struct Type));
      fun_type->type = FUN_TYPE;
      fun_type->type_data.fun_type.return_type = base_type;
      fun_type->type_data.fun_type.param_types = declarator->data.function_type->params;
      result = process_abstract_declarator(declarator->data.function_type->next, fun_type);
      break;
    default:
      result = NULL; // should never happen
  }
  return result;
}

// parse and process a local type (type specifiers + abstract declarator)
// no extern/static storage class specifiers allowed
struct Type* parse_local_type(){
  struct Token* old_current = current;
  struct Type* base_type = parse_param_type();
  if (base_type == NULL){
    current = old_current;
    return NULL;
  }
  struct AbstractDeclarator* declarator = parse_abstract_declarator();
  if (declarator == NULL){
    current = old_current;
    return NULL;
  }

  struct Type* derived_type = process_abstract_declarator(declarator, base_type);

  return derived_type;
}

// Purpose: Parse a cast expression.
// Inputs: Consumes tokens starting at '(' type ')' and a cast operand.
// Outputs: Returns a CAST expression or NULL if the pattern does not match.
// Invariants/Assumptions: Uses parse_param_type to parse the target type.
struct Expr* parse_cast(){
  struct Token* old_current = current;
  if (!consume(OPEN_P)) return NULL;
  char* cast_loc = (current - 1)->start;

  struct Type* derived_type = parse_local_type();

  if (!consume(CLOSE_P) || derived_type == NULL){
    current = old_current;
    return NULL;
  }

  struct Expr* expr = parse_factor();
  if (expr == NULL){
    current = old_current;
    return NULL;
  }

  struct CastExpr cast = {derived_type, expr};
  struct Expr* result = alloc_expr(CAST, cast_loc);
  result->expr.cast_expr = cast;
  return result;
}

// Purpose: Parse a parenthesized expression.
// Inputs: Consumes '(' expr ')' from the token stream.
// Outputs: Returns the inner expression or NULL on failure.
// Invariants/Assumptions: Used to group expressions in the Pratt parser.
struct Expr* parse_parens(){
  struct Token* old_current = current;
  if (!consume(OPEN_P)) {
    return NULL;
  }

  {
    struct Token* after_paren = current;
    bool success = true;
    struct Block* block = parse_block(&success);

    if (success && consume(CLOSE_P)) {
      struct StmtExpr stmt_expr = {block};
      struct Expr* result = alloc_expr(STMT_EXPR, (current - 1)->start);
      result->expr.stmt_expr = stmt_expr;
      return result;
    }

    current = after_paren;
  }

  // Fall back to an ordinary parenthesized expression.
  struct Expr* inner = parse_expr();
  if (inner == NULL) {
    current = old_current;
    return NULL;
  } else if (!consume(CLOSE_P)){
    current = old_current;
    return NULL;
  }
  return inner;
}

// Purpose: Parse a comma-separated argument list.
// Inputs: Consumes expressions until a closing ')'.
// Outputs: Returns a linked list of ArgList nodes or NULL on failure.
// Invariants/Assumptions: Caller already consumed the opening '('.
struct ArgList* parse_args(){
  struct Expr* arg;
  struct Token* old_current = current;
  if ((arg = parse_assignment_expr())){
    struct ArgList* args = arena_alloc(sizeof(struct ArgList));
    args->arg = arg;
    if (consume(COMMA)) args->next = parse_args();
    else if (consume(CLOSE_P)) args->next = NULL;
    else {
      current = old_current;
      return NULL;
    }
    return args;
  } else return NULL;
}

// Purpose: Parse prefix unary operators and address/deref expressions.
// Inputs: Consumes tokens from the current cursor.
// Outputs: Returns an expression node or NULL if no unary form matches.
// Invariants/Assumptions: Handles ++/-- via parse_pre_op.
struct Expr* parse_unary(){
  enum UnOp op;
  struct Token* old_current = current;
  if ((op = consume_unary_op())){
    char* op_loc = (current - 1)->start;
    struct Expr* inner = parse_factor();
    if (inner == NULL) {
      current = old_current - 1;
      return NULL;
    }
    struct UnaryExpr expr = {op, inner};
    struct Expr* result = alloc_expr(UNARY, op_loc);
    result->expr.un_expr = expr;
    return result;
  }

  struct Expr* expr;
  if ((expr = parse_pre_op())){
    return expr;
  } else if (consume(ASTERISK)){ 
    char* op_loc = (current - 1)->start;
    struct Token* old_current = current - 1;
    struct Expr* inner = parse_factor();
    if (inner == NULL) {
      current = old_current;
      return NULL;
    }
    struct DereferenceExpr expr = {inner};
    struct Expr* result = alloc_expr(DEREFERENCE, op_loc);
    result->expr.deref_expr = expr;
    return result;
  } else if (consume(AMPERSAND)){
    char* op_loc = (current - 1)->start;
    struct Token* old_current = current - 1;
    struct Expr* inner = parse_factor();
    if (inner == NULL) {
      current = old_current;
      return NULL;
    }
    struct AddrOfExpr expr = {inner};
    struct Expr* result = alloc_expr(ADDR_OF, op_loc);
    result->expr.addr_of_expr = expr;
    return result;
  }
  return NULL;
}

struct LitExpr parse_lit_expr(void){
  union TokenVariant* data;
  if ((data = consume_with_data(INT_LIT))){
    union ConstVariant const_data;
    const_data.int_val = data->int_val;
    struct LitExpr lit_expr = {INT_CONST, const_data};

    return lit_expr;
  } else if ((data = consume_with_data(U_INT_LIT))){
    union ConstVariant const_data;
    const_data.uint_val = data->uint_val;
    struct LitExpr lit_expr = {UINT_CONST, const_data};

    return lit_expr;
  } else if ((data = consume_with_data(CHAR_LIT))){
    union ConstVariant const_data;
    const_data.int_val = data->char_val;
    struct LitExpr lit_expr = {INT_CONST, const_data};

    return lit_expr;
  } else {
    union ConstVariant const_data;
    const_data.int_val = 0;
    struct LitExpr lit_expr = {-1, const_data};
    return lit_expr;
  }
}

// Purpose: Append a raw string literal slice into an output buffer, honoring escapes.
// Inputs: out is the destination buffer; out_index is the current write offset; raw is the source slice.
// Outputs: Returns the updated out_index after appending the escaped characters.
// Invariants/Assumptions: out has enough capacity for raw->len characters.
static size_t append_escaped_string(char* out, size_t out_index,
                                    struct Slice* raw) {
  for (size_t i = 0; i < raw->len; i++) {
    if (raw->start[i] == '\\') {
      i++;
      switch (raw->start[i]) {
        case '\'':
          out[out_index++] = '\'';
          break;
        case '\"':
          out[out_index++] = '\"';
          break;
        case '\?':
          out[out_index++] = '\?';
          break;
        case '\\':
          out[out_index++] = '\\';
          break;
        case 'a':
          out[out_index++] = '\a';
          break;
        case 'b':
          out[out_index++] = '\b';
          break;
        case 'f':
          out[out_index++] = '\f';
          break;
        case 'n':
          out[out_index++] = '\n';
          break;
        case 'r':
          out[out_index++] = '\r';
          break;
        case 't':
          out[out_index++] = '\t';
          break;
        case 'v':
          out[out_index++] = '\v';
          break;
        case '0':
          out[out_index++] = '\0';
          break;
        case 'x': {
          size_t digits = 0;
          unsigned char value = 0;
          if (!decode_hex_escape(raw->start + i + 1, &digits, &value)) {
            if (digits == 0) {
              parse_error_at(raw->start + i,
                             "expected at least one hexadecimal digit after \\x");
            } else {
              parse_error_at(raw->start + i + 1 + digits,
                             "hex escape exceeds byte value 0xff");
            }
            exit(1);
          }
          out[out_index++] = (char)value;
          i += digits;
          break;
        }
        default:
          parse_error_at(raw->start + i, "invalid escape sequence");
          exit(1);
      }
    } else {
      out[out_index++] = raw->start[i];
    }
  }
  return out_index;
}

struct Expr* parse_string(void){
  union TokenVariant* data;
  if ((data = consume_with_data(STRING_LIT))){
    char* str_loc = (current - 1)->start;

    size_t total_raw_len = data->string_val->len;
    struct Token* lookahead = current;
    while (token_index(lookahead) < prog_size &&
           lookahead->type == STRING_LIT) {
      total_raw_len += lookahead->data.string_val->len;
      lookahead++;
    }

    char* escaped_str = arena_alloc(total_raw_len + 1);
    size_t esc_index = 0;
    esc_index = append_escaped_string(escaped_str, esc_index, data->string_val);

    while (token_index(current) < prog_size &&
           current->type == STRING_LIT) {
      union TokenVariant* next_data = consume_with_data(STRING_LIT);
      esc_index = append_escaped_string(escaped_str, esc_index, next_data->string_val);
    }
    escaped_str[esc_index] = '\0';

    struct Slice* escaped_slice = arena_alloc(sizeof(struct Slice));
    escaped_slice->start = escaped_str;
    escaped_slice->len = esc_index;

    struct StringExpr string_expr = {escaped_slice};

    struct Expr* expr = alloc_expr(STRING, str_loc);
    expr->expr.string_expr = string_expr;
    return expr;
  } else {
    return NULL;
  }
}

struct Expr* parse_postfix() {
  // postfix can be a primary expr followed by
  // function calls, array subscripts, postfix ++/--
  // or struct . and ->
  struct Token* old_current = current;
  struct Expr* expr = parse_primary_expr();
  if (expr == NULL) return NULL;

  while (true){
    if (consume(OPEN_P)){
      char* call_loc = (current - 1)->start;
      struct ArgList* args;
      if (consume(CLOSE_P)) args = NULL;
      else {
        args = parse_args();
        if (args == NULL){
          current = old_current;
          return NULL;
        }
      }
      struct FunctionCallExpr call = {expr, args};
      struct Expr* new_expr = alloc_expr(FUNCTION_CALL, call_loc);
      new_expr->expr.fun_call_expr = call;
      expr = new_expr;
    } else if (consume(OPEN_S)){
      struct Expr* index = parse_expr();
      if (index == NULL || !consume(CLOSE_S)){
        current = old_current;
        return NULL;
      }
      struct SubscriptExpr subscript_expr = {expr, index};
      struct Expr* new_expr = alloc_expr(SUBSCRIPT, (current - 1)->start);
      new_expr->expr.subscript_expr = subscript_expr;
      expr = new_expr;
    } else if (consume(INC_TOK)) {
      char* op_loc = (current - 1)->start;
      struct PostAssignExpr add_one = {POST_INC, expr};
      struct Expr* new_expr = alloc_expr(POST_ASSIGN, op_loc);
      new_expr->expr.post_assign_expr = add_one;
      expr = new_expr;
    } else if (consume(DEC_TOK)) {
      char* op_loc = (current - 1)->start;
      struct PostAssignExpr sub_one = {POST_DEC, expr};
      struct Expr* new_expr = alloc_expr(POST_ASSIGN, op_loc);
      new_expr->expr.post_assign_expr = sub_one;
      expr = new_expr;
    } else if (consume(DOT_TOK)){
      char* dot_loc = (current - 1)->start;
      struct Token* old_current = current - 1;
      union TokenVariant* data = consume_with_data(IDENT);
      if (data == NULL){
        current = old_current;
        return NULL;
      }
      struct DotExpr member_access = {expr, data->ident_name};
      struct Expr* new_expr = alloc_expr(DOT_EXPR, dot_loc);
      new_expr->expr.dot_expr = member_access;
      expr = new_expr;
    } else if (consume(ARROW_TOK)){
      char* arrow_loc = (current - 1)->start;
      struct Token* old_current = current - 1;
      union TokenVariant* data = consume_with_data(IDENT);
      if (data == NULL){
        current = old_current;
        return NULL;
      }
      struct ArrowExpr ptr_member_access = {expr, data->ident_name};
      struct Expr* new_expr = alloc_expr(ARROW_EXPR, arrow_loc);
      new_expr->expr.arrow_expr = ptr_member_access;
      expr = new_expr;
    } else {
      break;
    }
  }

  return expr;
}

struct Expr* parse_sizeof(){
  struct Token* start = current;
  if (!consume(SIZEOF_TOK)) return NULL;
  
  struct Expr* expr = NULL;
  if ((expr = parse_sub_factor())){
    // sizeof (<expr>)
    struct Expr* sizeof_expr = arena_alloc(sizeof(struct Expr));
    sizeof_expr->loc = start->start;
    sizeof_expr->type = SIZEOF_EXPR;
    sizeof_expr->expr.sizeof_expr.expr = expr;
    return sizeof_expr;
  }

  // sizeof (<type>)
  if (!consume(OPEN_P)){
    current = start;
    return NULL;
  }

  // parse type
  struct Type* type = parse_local_type();
  if (type == NULL){
    current = start;
    return NULL;
  }

  if (!consume(CLOSE_P)){
    current = start;
    return NULL;
  }

  struct Expr* sizeof_t_expr = arena_alloc(sizeof(struct Expr));
  sizeof_t_expr->loc = start->start;
  sizeof_t_expr->type = SIZEOF_T_EXPR;
  sizeof_t_expr->expr.sizeof_t_expr.type = type;
  return sizeof_t_expr;
}

struct Expr* parse_sub_factor(){
  // same as factor but without casts
  // used because sizeof accepts this as an argument, but not casts
  struct Expr* expr = NULL;
  if ((expr = parse_unary())) return expr;
  else if ((expr = parse_sizeof())) return expr;
  else if ((expr = parse_postfix())) return expr;
  else return NULL;
}

struct Expr* parse_factor(){
  struct Expr* expr = NULL;
  if ((expr = parse_cast())) return expr;
  else return parse_sub_factor();
}

// Purpose: Parse a primary expression or literal.
// Inputs: Consumes literals, casts, unary ops, calls, or variables.
// Outputs: Returns an expression node or NULL if parsing fails.
// Invariants/Assumptions: This is the base case for the Pratt parser.
struct Expr* parse_primary_expr(){
  
  struct LitExpr lit_expr = parse_lit_expr();
  if (lit_expr.type != -1){
    struct Expr* expr = alloc_expr(LIT, (current - 1)->start);
    expr->expr.lit_expr = lit_expr;
    return expr;
  }
  
  struct Expr* expr;
  if ((expr = parse_parens())) return expr;
  else if ((expr = parse_var())) return expr;
  else if ((expr = parse_string())) return expr;
  else return NULL;
}

// Purpose: Map a binary operator to its precedence level.
// Inputs: op is the binary operator enum.
// Outputs: Returns a numeric precedence value (higher binds tighter).
// Invariants/Assumptions: Precedence values match the parser's associativity rules.
static unsigned get_prec(enum BinOp op){
  switch (op){
    case DIV_OP:
    case MUL_OP:
    case MOD_OP:
      return 60;
    case ADD_OP:
    case SUB_OP:
      return 55;
    case BIT_SHL:
    case BIT_SHR:
      return 50;
    case BOOL_LE:
    case BOOL_GE:
    case BOOL_LEQ:
    case BOOL_GEQ:
      return 45;
    case BOOL_EQ:
    case BOOL_NEQ:
      return 40;
    case BIT_AND:
      return 35;
    case BIT_XOR:
      return 30;
    case BIT_OR:
      return 25;
    case BOOL_AND:
      return 20;
    case BOOL_OR:
      return 15;
    case TERNARY_OP:
      return 10;
    case ASSIGN_OP:
    case PLUS_EQ_OP:
    case MINUS_EQ_OP:
    case MUL_EQ_OP:
    case DIV_EQ_OP:
    case MOD_EQ_OP:
    case AND_EQ_OP:
    case OR_EQ_OP:
    case XOR_EQ_OP:
    case SHL_EQ_OP:
    case SHR_EQ_OP:
      return 5;
    case COMMA_OP:
      return 3;
  }
  return 0;
}

// Purpose: Parse binary and ternary expressions with Pratt parsing.
// Inputs: min_prec is the current binding power threshold.
// Outputs: Returns an expression tree or NULL on failure.
// Invariants/Assumptions: Assignment and ternary operators are right-associative.
struct Expr* parse_bin_expr(unsigned min_prec){
  struct Token* old_current = current;
  struct Expr* lhs = parse_factor();

  if (lhs == NULL) return NULL;

  while (token_index(current) < prog_size){ 
    enum BinOp op = consume_binary_op();

    if (op == 0) {
      return lhs; 
    } 

    char* op_loc = (current - 1)->start;
    unsigned prec = get_prec(op);

    if (prec < min_prec) {
      current--;
      break; // stop expansion if a lower precedence operator is encountered
    }

    if (op == TERNARY_OP){
      // Ternary is parsed as: lhs ? middle : rhs
      struct Expr* middle = parse_expr();
      if (middle == NULL) {
        current = old_current;
        return NULL;
      }
      if (!consume(COLON)){
        return NULL;
      }
      // RHS binds at the same precedence as the ternary operator.
      struct Expr* rhs = parse_bin_expr(prec);
      if (rhs == NULL) {
        current = old_current;
        return NULL;
      }
      struct ConditionalExpr conditional_expr = {lhs, middle, rhs};
      lhs = alloc_expr(CONDITIONAL, op_loc);
      lhs->expr.conditional_expr = conditional_expr;
      continue;
    }

    // Assignment/compound ops are right-associative, everything else is left-associative.
    unsigned next_prec = (ASSIGN_OP <= op && op <= SHR_EQ_OP) ? prec : prec + 1;
    struct Expr* rhs = parse_bin_expr(next_prec);

    if (rhs == NULL){
      current--;
      return NULL;
    }

    if (op == ASSIGN_OP){
      struct AssignExpr assign_expr = {lhs, rhs};
      lhs = alloc_expr(ASSIGN, op_loc);
      lhs->expr.assign_expr = assign_expr;
    } else {
      struct BinaryExpr bin_expr = {op, lhs, rhs};
      lhs = alloc_expr(BINARY, op_loc);
      lhs->expr.bin_expr = bin_expr;
    }
  }

  return lhs;
}

// Purpose: Parse a full expression, including comma operators.
// Inputs: Consumes tokens from the current cursor.
// Outputs: Returns an expression tree or NULL on failure.
// Invariants/Assumptions: Delegates precedence handling to parse_assignment_expr.
struct Expr* parse_expr(){
  return parse_bin_expr(0);
}

// Purpose: Parse an expression but treats commas as separators.
// Inputs: Consumes tokens from the current cursor.
// Outputs: Returns an expression tree or NULL on failure.
// Invariants/Assumptions: Delegates precedence handling to parse_bin_expr.
struct Expr* parse_assignment_expr(){
  return parse_bin_expr(4); // comma has precedence 3
}


// Purpose: Parse a return statement.
// Inputs: Consumes 'return' expr ';' from the token stream.
// Outputs: Returns a RETURN_STMT node or NULL on failure.
// Invariants/Assumptions: Expression parsing must succeed for valid returns.
struct Statement* parse_return_stmt(){
  struct Token* old_current = current;
  if (!consume(RETURN_TOK)) return NULL;
  char* stmt_loc = (current - 1)->start;
  struct Expr* expr = parse_expr();

  // don't check if expr is NULL, as return may not have an expr

  if (!consume(SEMI)){
    current = old_current;
    return NULL;
  }
  struct ReturnStmt ret_stmt = {expr, NULL};
  struct Statement* result = alloc_stmt(RETURN_STMT, stmt_loc);
  result->statement.ret_stmt = ret_stmt;
  return result;
}

// Purpose: Parse an expression statement.
// Inputs: Consumes an expression followed by ';'.
// Outputs: Returns an EXPR_STMT node or NULL on failure.
// Invariants/Assumptions: Empty statements are handled elsewhere.
struct Statement* parse_expr_stmt(){
  struct Token* old_current = current;
  struct Expr* expr = parse_expr();
  if (expr == NULL) return NULL;
  if (!consume(SEMI)){
    current = old_current;
    return NULL;
  }
  struct ExprStmt expr_stmt = {expr};
  struct Statement* result = alloc_stmt(EXPR_STMT, expr->loc);
  result->statement.expr_stmt = expr_stmt;
  return result;
}

// Purpose: Parse an if/else statement.
// Inputs: Consumes 'if' '(' expr ')' stmt ['else' stmt].
// Outputs: Returns an IF_STMT node or NULL on failure.
// Invariants/Assumptions: Nested statements are parsed via parse_statement.
struct Statement* parse_if_stmt(){
  struct Token* old_current = current;
  if (!consume(IF_TOK)) return NULL;
  char* stmt_loc = (current - 1)->start;
  if (!consume(OPEN_P)){
    current = old_current;
    return NULL;
  }
  struct Expr* condition = parse_expr();
  if (condition == NULL){
    current = old_current;
    return NULL;
  }
  if (!consume(CLOSE_P)) {
    current = old_current;
    return NULL;
  }
  struct Statement* left = parse_statement();
  if (left == NULL) {
    current = old_current;
    return NULL;
  }
  struct IfStmt if_stmt = {condition, left, NULL};
  struct Statement* result = alloc_stmt(IF_STMT, stmt_loc);
  result->statement.if_stmt = if_stmt;
  old_current = current;
  if (consume(ELSE_TOK)){
    struct Statement* right = parse_statement();
    if (right == NULL){
      current = old_current;
      return NULL;
    } else {
      result->statement.if_stmt.else_stmt = right;
    }
  }
  return result;
}

// Purpose: Parse a user-defined label statement.
// Inputs: Consumes IDENT ':' stmt.
// Outputs: Returns a LABELED_STMT node or NULL on failure.
// Invariants/Assumptions: Label resolution happens in a later pass.
struct Statement* parse_labeled_stmt(){
  struct Token* old_current = current;
  union TokenVariant* data = consume_with_data(IDENT);
  if (data == NULL) return NULL;
  char* stmt_loc = (current - 1)->start;
  struct Slice* label = data->ident_name;
  if (!consume(COLON)){
    current = old_current;
    return NULL;
  }
  struct Statement* stmt = parse_statement();
  if (stmt == NULL){
    current = old_current;
    return NULL;
  }

  struct LabeledStmt labeled_stmt = {label, stmt};
  struct Statement* result = alloc_stmt(LABELED_STMT, stmt_loc);
  result->statement.labeled_stmt = labeled_stmt;
  return result;
}

// Purpose: Parse a goto statement.
// Inputs: Consumes 'goto' IDENT ';'.
// Outputs: Returns a GOTO_STMT node or NULL on failure.
// Invariants/Assumptions: Label resolution happens in a later pass.
struct Statement* parse_goto_stmt(){
  struct Token* old_current = current;
  if (!consume(GOTO_TOK)) return NULL;
  char* stmt_loc = (current - 1)->start;
  union TokenVariant* data;
  if ((data = consume_with_data(IDENT)) && consume(SEMI)){
    struct GotoStmt goto_stmt = { data->ident_name };

    struct Statement* result = alloc_stmt(GOTO_STMT, stmt_loc);
    result->statement.goto_stmt = goto_stmt;
    return result;
  } else {
    current = old_current;
    return NULL; 
  }
}

// Block items are either a statement or a declaration.
// Purpose: Parse one block item (declaration or statement).
// Inputs: Consumes tokens from the current cursor.
// Outputs: Returns a BlockItem or NULL when parsing fails.
// Invariants/Assumptions: Declaration parsing is attempted before statements.
struct BlockItem* parse_block_item(){
  struct Statement* stmt = parse_statement();
  if (stmt != NULL){
    struct BlockItem* item = arena_alloc(sizeof(struct BlockItem));
    item->type = STMT_ITEM;
    item->item.stmt = stmt;
    return item;
  }
  struct Declaration* dclr = parse_declaration();
  if (dclr != NULL){
    struct BlockItem* item = arena_alloc(sizeof(struct BlockItem));
    item->type = DCLR_ITEM;
    item->item.dclr = dclr;
    return item;
  }
  return NULL; 
}

// Parse a { ... } block into a linked list of items.
// Purpose: Parse a sequence of block items until a closing brace.
// Inputs: success is set to false on parse failure.
// Outputs: Returns a linked list of Block nodes or NULL.
// Invariants/Assumptions: Caller handles scope entry/exit.
struct Block* parse_block(bool* success){
  *success = true;
  struct Token* old_current = current;

  if (!consume(OPEN_B)) {
    *success = false;
    return NULL;
  }

  struct Block* block = NULL;

  struct BlockItem* item = parse_block_item();

  if (item != NULL){
    block = arena_alloc(sizeof(struct Block));
    block->item = item;
    block->next = NULL;
    struct Block* prev_block = block;
    struct Block* cur_block;
    while ((item = parse_block_item()) != NULL){
      cur_block = arena_alloc(sizeof(struct Block));
      cur_block->item = item;
      cur_block->next = NULL;
      prev_block->next = cur_block;
      prev_block = cur_block;
    }
  }

  if (!consume(CLOSE_B)){
    current = old_current;
    *success = false;
    return NULL;
  }
  return block;
}

// Purpose: Parse a compound statement (block).
// Inputs: Consumes '{' block '}'.
// Outputs: Returns a COMPOUND_STMT node or NULL on failure.
// Invariants/Assumptions: Blocks may contain both declarations and statements.
struct Statement* parse_compound_stmt(){
  bool success;
  if (token_index(current) >= prog_size || current->type != OPEN_B) {
    return NULL;
  }
  char* stmt_loc = current->start;
  // output parameter because returning NULL could mean failure or empty block
  struct Block* block = parse_block(&success);
  if (!success) return NULL;
  struct CompoundStmt compound_stmt = { block };
  struct Statement* result = alloc_stmt(COMPOUND_STMT, stmt_loc);
  result->statement.compound_stmt = compound_stmt;
  return result;
}

// Purpose: Parse a break statement.
// Inputs: Consumes 'break' ';'.
// Outputs: Returns a BREAK_STMT node or NULL on failure.
// Invariants/Assumptions: Loop/switch validation occurs in later passes.
struct Statement* parse_break_stmt(){
  struct Token* old_current = current;
  if (!consume(BREAK_TOK)) return NULL;
  char* stmt_loc = (current - 1)->start;
  if (!consume(SEMI)) {
    current = old_current;
    return NULL;
  }
  struct BreakStmt break_stmt = {NULL};
  struct Statement* result = alloc_stmt(BREAK_STMT, stmt_loc);
  result->statement.break_stmt = break_stmt;
  return result;
}

// Purpose: Parse a continue statement.
// Inputs: Consumes 'continue' ';'.
// Outputs: Returns a CONTINUE_STMT node or NULL on failure.
// Invariants/Assumptions: Loop validation occurs in later passes.
struct Statement* parse_continue_stmt(){
  struct Token* old_current = current;
  if (!consume(CONTINUE_TOK)) return NULL;
  char* stmt_loc = (current - 1)->start;
  if (!consume(SEMI)) {
    current = old_current;
    return NULL;
  }
  struct ContinueStmt continue_stmt = {NULL};
  struct Statement* result = alloc_stmt(CONTINUE_STMT, stmt_loc);
  result->statement.continue_stmt = continue_stmt;
  return result;
}

// Purpose: Parse a while loop.
// Inputs: Consumes 'while' '(' expr ')' stmt.
// Outputs: Returns a WHILE_STMT node or NULL on failure.
// Invariants/Assumptions: Loop labeling happens in a later pass.
struct Statement* parse_while_stmt(){
  struct Token* old_current = current;
  if (!consume(WHILE_TOK)) return NULL;
  char* stmt_loc = (current - 1)->start;
  if (!consume(OPEN_P)){
    current = old_current;
    return NULL;
  }
  struct Expr* condition = parse_expr();
  if (condition == NULL){
    current = old_current;
    return NULL;
  }
  if (!consume(CLOSE_P)){
    current = old_current;
    return NULL;
  }
  struct Statement* stmt = parse_statement();
  if (stmt == NULL){
    current = old_current;
    return NULL;
  }

  struct WhileStmt while_stmt = {condition, stmt, NULL};
  struct Statement* result = alloc_stmt(WHILE_STMT, stmt_loc);
  result->statement.while_stmt = while_stmt;
  return result;
}

// Purpose: Parse a do-while loop.
// Inputs: Consumes 'do' stmt 'while' '(' expr ')' ';'.
// Outputs: Returns a DO_WHILE_STMT node or NULL on failure.
// Invariants/Assumptions: Loop labeling happens in a later pass.
struct Statement* parse_do_while_stmt(){
  struct Token* old_current = current;
  if (!consume(DO_TOK)) return NULL;
  char* stmt_loc = (current - 1)->start;
  struct Statement* stmt = parse_statement();
  if (stmt == NULL){
    current = old_current;
    return NULL;
  }
  if (!consume(WHILE_TOK)){
    current = old_current;
    return NULL;
  }
  if (!consume(OPEN_P)){
    current = old_current;
    return NULL;
  }
  struct Expr* condition = parse_expr();
  if (condition == NULL){
    current = old_current;
    return NULL;
  }
  if (!consume(CLOSE_P) || !consume(SEMI)){
    current = old_current;
    return NULL;
  }

  struct DoWhileStmt do_while_stmt = {stmt, condition, NULL};
  struct Statement* result = alloc_stmt(DO_WHILE_STMT, stmt_loc);
  result->statement.do_while_stmt = do_while_stmt;
  return result;
}

bool is_type_specifier(enum TokenType type){
  switch (current->type) {
    case INT_TOK:
    case SIGNED_TOK:
    case UNSIGNED_TOK:
    case LONG_TOK:
    case SHORT_TOK:
    case CHAR_TOK:
    case STATIC_TOK:
    case EXTERN_TOK:
    case VOID_TOK:
    case STRUCT_TOK:
    case UNION_TOK:
    case ENUM_TOK:
      return true;
    default:
      return false;
  }
}

// Purpose: Parse the declaration form of a for-loop initializer.
// Inputs: Consumes a type/specifier sequence and declarator.
// Outputs: Returns a VariableDclr or NULL if no declaration is found.
// Invariants/Assumptions: Only simple variable declarators are accepted here.
struct VariableDclr* parse_for_dclr(){
  struct Token* old_current = current;
  if (token_index(current) >= prog_size) {
    return NULL;
  }

  // check for attributes
  struct VarAttributes* attrs = NULL;
  if ((attrs = parse_var_attributes()) == NULL) return NULL; // possible attribute location 1

  if (!is_type_specifier(current->type)) {
    return NULL;
  }

  struct Type* base_type = NULL;
  enum StorageClass storage = NONE;
  parse_type_and_storage_class(&base_type, &storage);
  if (base_type == NULL) {
    current = old_current;
    return NULL;
  }

  struct VarAttributes* new_attrs = NULL;
  if ((new_attrs = parse_var_attributes()) == NULL) return NULL; // possible attribute location 2
  if (attrs->cleanup_func != NULL && new_attrs->cleanup_func != NULL){
    parse_error_at(parser_error_ptr(), "cleanup specified multiple times");
    return NULL;
  }
  if (new_attrs->cleanup_func != NULL){
    attrs = new_attrs;
  }

  struct Declarator* declarator = parse_declarator();
  if (declarator == NULL) {
    parse_error_at(parser_error_ptr(), "invalid declarator in for-loop initializer");
    return NULL;
  }

  if ((new_attrs = parse_var_attributes()) == NULL) return NULL; // possible attribute location 3
  if (attrs->cleanup_func != NULL && new_attrs->cleanup_func != NULL){
    parse_error_at(parser_error_ptr(), "cleanup specified multiple times");
    return NULL;
  }
  if (new_attrs->cleanup_func != NULL){
    attrs = new_attrs;
  }

  struct Slice* name = NULL;
  struct Type* decl_type = NULL;
  struct ParamList* params = NULL;
  if (!process_declarator(declarator, base_type, &name, &decl_type, &params)) {
    current = old_current;
    return NULL;
  }
  if (decl_type->type == FUN_TYPE) {
    current = old_current;
    return NULL;
  }

  struct VariableDclr* var_dclr = parse_var_dclr(decl_type, storage, name);
  if (var_dclr == NULL || !consume(SEMI)) {
    current = old_current;
    return NULL;
  }
  var_dclr->attributes = *attrs;
  return var_dclr;
}

// Parse for-loop initializer, preferring a declaration when possible.
// Purpose: Parse the initializer portion of a for-loop.
// Inputs: Consumes either a declaration or an optional expression.
// Outputs: Returns a ForInit node or NULL on failure.
// Invariants/Assumptions: The semicolon after init is consumed here.
struct ForInit* parse_for_init(){
  struct Token* old_current = current;
  struct VariableDclr* var_dclr = parse_for_dclr();
  if (var_dclr != NULL){
    struct ForInit* init = arena_alloc(sizeof(struct ForInit));
    init->type = DCLR_INIT;
    init->init.dclr_init = var_dclr;
    return init;
  } else {
    struct Expr* expr_init = parse_expr();
    if (!consume(SEMI)){
      current = old_current;
      return NULL;
    }
    struct ForInit* init = arena_alloc(sizeof(struct ForInit));
    init->type = EXPR_INIT;
    init->init.expr_init = expr_init;
    return init;
  }
}

// Purpose: Parse a for loop statement.
// Inputs: Consumes 'for' '(' init ';' cond ';' end ')' stmt.
// Outputs: Returns a FOR_STMT node or NULL on failure.
// Invariants/Assumptions: init parsing consumes the first semicolon.
struct Statement* parse_for_stmt(){
  struct Token* old_current = current;
  if (!consume(FOR_TOK)) return NULL;
  char* stmt_loc = (current - 1)->start;
  if (!consume(OPEN_P)){
    current = old_current;
    return NULL;
  }
  struct ForInit* init = parse_for_init();
  if (init == NULL){
    current = old_current;
    return NULL;
  }
  struct Expr* condition = parse_expr();
  if (!consume(SEMI)){
    current = old_current;
    return NULL;
  }
  struct Expr* end = parse_expr();
  if (!consume(CLOSE_P)){
    current = old_current;
    return NULL;
  }
  struct Statement* stmt = parse_statement();
  if (stmt == NULL){
    current = old_current;
    return NULL;
  }

  struct ForStmt for_stmt = {init, condition, end, stmt, NULL};
  struct Statement* result = alloc_stmt(FOR_STMT, stmt_loc);
  result->statement.for_stmt = for_stmt;
  return result;
}

// Purpose: Parse a switch statement.
// Inputs: Consumes 'switch' '(' expr ')' stmt.
// Outputs: Returns a SWITCH_STMT node or NULL on failure.
// Invariants/Assumptions: Case collection and labeling happen later.
struct Statement* parse_switch_stmt(){
  struct Token* old_current = current;
  if (!consume(SWITCH_TOK)) return NULL;
  char* stmt_loc = (current - 1)->start;
  if (!consume(OPEN_P)){
    current = old_current;
    return NULL;
  }
  struct Expr* condition = parse_expr();
  if (condition == NULL){
    current = old_current;
    return NULL;
  }
  if (!consume(CLOSE_P)){
    current = old_current;
    return NULL;
  }
  struct Statement* stmt = parse_statement();
  if (stmt == NULL){
    current = old_current;
    return NULL;
  }
  struct SwitchStmt switch_stmt = {condition, stmt, NULL, NULL};
  struct Statement* result = alloc_stmt(SWITCH_STMT, stmt_loc);
  result->statement.switch_stmt = switch_stmt;
  return result;
}

// Purpose: Parse a case label statement.
// Inputs: Consumes 'case' expr ':' stmt.
// Outputs: Returns a CASE_STMT node or NULL on failure.
// Invariants/Assumptions: Case validity is checked in later passes.
struct Statement* parse_case_stmt(){
  struct Token* old_current = current;
  if (!consume(CASE_TOK)) return NULL;
  char* stmt_loc = (current - 1)->start;
  struct Expr* expr = parse_expr();
  if (expr == NULL){
    current = old_current;
    return NULL;
  }
  if (!consume(COLON)){
    current = old_current;
    return NULL;
  }
  struct Statement* stmt = parse_statement();
  if (stmt == NULL){
    current = old_current;
    return NULL;
  }
  struct CaseStmt case_stmt = {expr, stmt, NULL};
  struct Statement* result = alloc_stmt(CASE_STMT, stmt_loc);
  result->statement.case_stmt = case_stmt;
  return result;
}

// Purpose: Parse a default label statement.
// Inputs: Consumes 'default' ':' stmt.
// Outputs: Returns a DEFAULT_STMT node or NULL on failure.
// Invariants/Assumptions: Default validity is checked in later passes.
struct Statement* parse_default_stmt(){
  struct Token* old_current = current;
  if (!consume(DEFAULT_TOK)) return NULL;
  char* stmt_loc = (current - 1)->start;
  if (!consume(COLON)){
    current = old_current;
    return NULL;
  }
  struct Statement* stmt = parse_statement();
  if (stmt == NULL){
    current = old_current;
    return NULL;
  }
  struct DefaultStmt default_stmt = {stmt, NULL};
  struct Statement* result = alloc_stmt(DEFAULT_STMT, stmt_loc);
  result->statement.default_stmt = default_stmt;
  return result;
}

// Purpose: Parse an empty statement.
// Inputs: Consumes a single ';' token.
// Outputs: Returns a NULL_STMT node or NULL on failure.
// Invariants/Assumptions: Empty statements are distinct from expression statements.
struct Statement* parse_null_stmt(){
  if (consume(SEMI)){
    struct NullStmt null_stmt;
    struct Statement* result = alloc_stmt(NULL_STMT, (current - 1)->start);
    result->statement.null_stmt = null_stmt;
    return result;
  } else return NULL;
}

// Statement dispatcher; order matters to resolve ambiguities.
// Purpose: Parse any statement form.
// Inputs: Consumes tokens from the current cursor.
// Outputs: Returns a Statement node or NULL on failure.
// Invariants/Assumptions: Statement parsing is ordered by keyword precedence.
struct Statement* parse_statement(){
  struct Statement* stmt;
  if ((stmt = parse_return_stmt())) return stmt;
  else if ((stmt = parse_expr_stmt())) return stmt;
  else if ((stmt = parse_if_stmt())) return stmt;
  else if ((stmt = parse_labeled_stmt())) return stmt;
  else if ((stmt = parse_goto_stmt())) return stmt;
  else if ((stmt = parse_compound_stmt())) return stmt;
  else if ((stmt = parse_break_stmt())) return stmt;
  else if ((stmt = parse_continue_stmt())) return stmt;
  else if ((stmt = parse_while_stmt())) return stmt;
  else if ((stmt = parse_do_while_stmt())) return stmt;
  else if ((stmt = parse_for_stmt())) return stmt;
  else if ((stmt = parse_switch_stmt())) return stmt;
  else if ((stmt = parse_case_stmt())) return stmt;
  else if ((stmt = parse_default_stmt())) return stmt;
  else if ((stmt = parse_null_stmt())) return stmt;
  else return NULL;
}

// Purpose: Parse an initializer without relying on type information.
// Inputs: Consumes tokens from the current cursor.
// Outputs: Returns an Initializer node or NULL on failure.
// Invariants/Assumptions: Does not enforce type validity; caller must validate.
static struct Initializer* parse_initializer_any(void) {
  struct Token* old_current = current;

  struct Expr* expr = parse_assignment_expr();
  if (expr != NULL) {
    struct Initializer* init = arena_alloc(sizeof(struct Initializer));
    init->init_type = SINGLE_INIT;
    init->init.single_init = expr;
    init->loc = expr->loc;
    init->type = NULL;
    return init;
  }
  current = old_current;

  if (!consume(OPEN_B)) {
    current = old_current;
    return NULL;
  }

  char* init_loc = (current - 1)->start;
  struct InitializerList* init_list = NULL;
  struct InitializerList* prev_init = NULL;
  struct Initializer* init = NULL;
  while ((init = parse_initializer_any()) != NULL) {
    struct InitializerList* next_init = arena_alloc(sizeof(struct InitializerList));
    next_init->init = init;
    next_init->next = NULL;
    if (init_list == NULL) {
      init_list = next_init;
    } else {
      prev_init->next = next_init;
    }
    prev_init = next_init;
    if (!consume(COMMA)) break;
  }
  if (init_list == NULL) {
    parse_error_at(init_loc, "initializer list must have at least one element");
    current = old_current;
    return NULL;
  }
  if (!consume(CLOSE_B)) {
    current = old_current;
    return NULL;
  }

  struct Initializer* compound_init = arena_alloc(sizeof(struct Initializer));
  compound_init->init_type = COMPOUND_INIT;
  compound_init->init.compound_init = init_list;
  compound_init->loc = init_loc;
  compound_init->type = NULL;

  return compound_init;
}

// Purpose: Parse a variable initializer with a known target type.
// Inputs: type is the target type for the initializer.
// Outputs: Returns an Initializer node or NULL on failure.
// Invariants/Assumptions: Only array/struct/union types allow compound initializers.
struct Initializer* parse_var_init(struct Type* type){
  struct Token* old_current = current;
  struct Initializer* init = parse_initializer_any();
  if (init == NULL) {
    current = old_current;
    return NULL;
  }

  if (init->init_type == COMPOUND_INIT &&
      type->type != ARRAY_TYPE && type->type != STRUCT_TYPE && type->type != UNION_TYPE) {
    parse_error_at(init->loc,
                   "Compound initializers are only supported for array, struct, and union types.");
    current = old_current;
    return NULL;
  }

  init->type = type;
  return init;
}

// Purpose: Parse a variable declarator with optional initializer.
// Inputs: type/storage/name come from earlier declarator parsing.
// Outputs: Returns a VariableDclr node or NULL on failure.
// Invariants/Assumptions: name is a valid identifier slice.
struct VariableDclr* parse_var_dclr(struct Type* type, enum StorageClass storage, struct Slice* name){
  struct Token* old_current = current;
  struct Initializer* init = NULL;
  if (consume(EQUALS)) {
    init = parse_var_init(type);
    if (init == NULL){
      current = old_current;
      return NULL;
    }
  }
  struct VariableDclr* var_dclr = arena_alloc(sizeof(struct VariableDclr));
  var_dclr->init = init;
  var_dclr->name = name;
  var_dclr->type = type;
  var_dclr->storage = storage;
  var_dclr->attributes.cleanup_func = NULL;
  return var_dclr;
}

// Purpose: Parse a type or storage-class prefix specifier.
// Inputs: Consumes tokens from the current cursor.
// Outputs: Returns a DclrPrefix node or NULL if no prefix matches.
// Invariants/Assumptions: Only single specifiers are parsed per call.
struct DclrPrefix* parse_type_or_storage_class(){
  if (consume(STATIC_TOK)){
    struct DclrPrefix* dclr_prefix = arena_alloc(sizeof(struct DclrPrefix));
    dclr_prefix->type = STORAGE_PREFIX;
    dclr_prefix->prefix.storage_class = STATIC;
    return dclr_prefix;
  }
  else if (consume(EXTERN_TOK)){
    struct DclrPrefix* dclr_prefix = arena_alloc(sizeof(struct DclrPrefix));
    dclr_prefix->type = STORAGE_PREFIX;
    dclr_prefix->prefix.storage_class = EXTERN;
    return dclr_prefix;
  }
  struct TypeSpecifier spec = parse_type_spec();
  if (spec.type != -1){
    struct DclrPrefix* dclr_prefix = arena_alloc(sizeof(struct DclrPrefix));
    dclr_prefix->type = TYPE_PREFIX;
    dclr_prefix->prefix.type_spec = spec;
    return dclr_prefix;
  }
  return NULL;
}

// Collect a sequence of type and storage keywords into lists.
void parse_types_and_storage_classes(struct StorageClassList** storages_result, struct TypeSpecList** type_specs_result){
  struct DclrPrefix* prefix;
  struct TypeSpecList* specs = NULL;
  struct StorageClassList* storages = NULL;

  struct TypeSpecList* prev_specs = NULL;
  struct StorageClassList* prev_storages = NULL;
  while ((prefix = parse_type_or_storage_class())){
    switch(prefix->type){
      case STORAGE_PREFIX: {
        struct StorageClassList* next_storage = arena_alloc(sizeof(struct StorageClassList));
        next_storage->spec = prefix->prefix.storage_class;
        next_storage->next = NULL;
        if (prev_storages != NULL){
          parse_error_at(parser_error_ptr(), "duplicate storage class specifier");
          exit(1);
        } else {
          storages = next_storage;
        }
        prev_storages = next_storage;
        break;
      }
      case TYPE_PREFIX: {
        struct TypeSpecList* next_spec = arena_alloc(sizeof(struct TypeSpecList));
        next_spec->spec = prefix->prefix.type_spec;
        next_spec->next = NULL;
        if (prev_specs != NULL){
          prev_specs->next = next_spec;
        } else {
          specs = next_spec;
        }
        prev_specs = next_spec;
        break;
      }
    }
  }
  *storages_result = storages;
  *type_specs_result = specs;
}

// Validate specifiers and build the base type + storage class.
void parse_type_and_storage_class(struct Type** type, enum StorageClass* class){
  struct StorageClassList* storages = NULL;
  struct TypeSpecList* specs = NULL;
  parse_types_and_storage_classes(&storages, &specs);

  *class = (storages == NULL) ? NONE : storages->spec;
  *type = type_spec_to_type(specs);
  return;
}

// Parse a declarator (possibly pointer-qualified).
// Recurses on leading '*' to build nested pointer declarators.
// Purpose: Parse a declarator with possible pointer and function syntax.
// Inputs: Consumes tokens from the current cursor.
// Outputs: Returns a Declarator node or NULL on failure.
// Invariants/Assumptions: Base type parsing happens separately.
struct Declarator* parse_declarator(){
  struct Token* old_current = current;
  if (consume(ASTERISK)){
    struct Declarator* decl = parse_declarator();
    if (decl != NULL){
      struct Declarator* result = arena_alloc(sizeof(struct Declarator));
      result->type = POINTER_DEC;
      result->declarator.pointer_dec.decl = decl;
      return result;
    } else {
      current = old_current;
      return NULL;
    }
  }
  return parse_direct_declarator();
}

// Parse an identifier or parenthesized declarator for grouping.
// Purpose: Parse a simple declarator (identifier or pointer chain).
// Inputs: Consumes tokens from the current cursor.
// Outputs: Returns a Declarator node or NULL on failure.
// Invariants/Assumptions: Function declarators are handled by parse_direct_declarator.
struct Declarator* parse_simple_declarator(){
  struct Token* old_current = current;
  union TokenVariant* data = consume_with_data(IDENT);
  if (data != NULL){
    // function or variable name
    struct Declarator* result = arena_alloc(sizeof(struct Declarator));
    result->type = IDENT_DEC;
    result->declarator.ident_dec.name = data->ident_name;
    return result;
  }
  if (consume(OPEN_P)){
    struct Declarator* result = parse_declarator();
    if (result == NULL){
      current = old_current;
      return NULL;
    }
    if (!consume(CLOSE_P)){
      current = old_current;
      return NULL;
    }
    return result;
  } 
  return NULL;
}

// Parse function parameter list into ParamInfo nodes.
// "(void)" and "()" both map to an empty parameter list sentinel.
// Purpose: Parse a parameter info list for function declarators.
// Inputs: Consumes parameter type/declarator tokens.
// Outputs: Returns a linked list of ParamInfoList nodes or NULL.
// Invariants/Assumptions: Parameter lists end with ')'.
struct ParamInfoList* parse_params(){
  struct Token* old_current = current;
  if (!consume(OPEN_P)) return NULL;
  struct Token* before_void = current;
  if (consume(VOID_TOK)){
    if (consume(CLOSE_P)){
      struct ParamInfoList* empty = arena_alloc(sizeof(struct ParamInfoList));
      empty->info.type = NULL;
      empty->info.decl.type = IDENT_DEC;
      empty->info.decl.declarator.ident_dec.name = NULL;
      empty->next = NULL;
      return empty;
    } else {
      current = before_void;
      // not the special case, rewind and see if its a real param
    }
  }
  if (consume(CLOSE_P)){
    struct ParamInfoList* empty = arena_alloc(sizeof(struct ParamInfoList));
    empty->info.type = NULL;
    empty->info.decl.type = IDENT_DEC;
    empty->info.decl.declarator.ident_dec.name = NULL;
    empty->next = NULL;
    return empty;
  }
  struct ParamInfoList* head = NULL;
  struct ParamInfoList** tail = &head;
  while (true){
    struct Type* type_ = parse_param_type();
    if (type_ == NULL){
      current = old_current;
      return NULL;
    }
    struct Declarator* declarator = parse_declarator();
    if (declarator == NULL){
      struct AbstractDeclarator* abstract_decl = parse_abstract_declarator();
      if (abstract_decl == NULL){
        current = old_current;
        return NULL;
      }
      struct Type* derived_type = process_abstract_declarator(abstract_decl, type_);
      struct Declarator* abstract_name = arena_alloc(sizeof(struct Declarator));
      abstract_name->type = IDENT_DEC;
      abstract_name->declarator.ident_dec.name = NULL;
      struct ParamInfoList* node = arena_alloc(sizeof(struct ParamInfoList));
      node->info.type = derived_type;
      node->info.decl = *abstract_name;
      node->next = NULL;
      *tail = node;
      tail = &node->next;
      if (consume(COMMA)) continue;
      if (consume(CLOSE_P)) break;
      current = old_current;
      return NULL;
    }
    struct ParamInfoList* node = arena_alloc(sizeof(struct ParamInfoList));
    node->info.type = type_;
    node->info.decl = *declarator;
    node->next = NULL;
    *tail = node;
    tail = &node->next;
    if (consume(COMMA)) continue;
    if (consume(CLOSE_P)) break;
    current = old_current;
    return NULL;
  }
  return head;
}

// Parse direct declarators and wrap in FUN_DEC when params follow.
// Purpose: Parse a direct declarator (identifier or function declarator).
// Inputs: Consumes tokens from the current cursor.
// Outputs: Returns a Declarator node or NULL on failure.
// Invariants/Assumptions: Nested declarators use parentheses.
struct Declarator* parse_direct_declarator(){
  struct Token* old_current = current;

  struct Declarator* decl = parse_simple_declarator();
  if (decl == NULL) return NULL;
  struct ParamInfoList* params = parse_params();
  if (params != NULL) {
    // function declarator
    struct FunDec fun_dec = {params, decl};
    struct Declarator* result = arena_alloc(sizeof(struct Declarator));
    result->type = FUN_DEC;
    result->declarator.fun_dec = fun_dec;
    return result;
  }

  bool is_array = false;
  size_t* array_sizes = NULL;
  size_t num_array_sizes = 0;
  size_t array_sizes_capacity = 16;
  while (consume(OPEN_S)) {
    is_array = true;
    if (array_sizes == NULL) {
      array_sizes = malloc(sizeof(size_t) * array_sizes_capacity); // initial capacity
    }
    // array declarator
    struct LitExpr size_expr = parse_lit_expr();
    if (size_expr.type == -1 || !consume(CLOSE_S)) {
      current = old_current;
      return NULL;
    }
    array_sizes[num_array_sizes++] = size_expr.value.uint_val;
    if (num_array_sizes >= array_sizes_capacity) {
      array_sizes_capacity *= 2;
      size_t* new_array = realloc(array_sizes, sizeof(size_t) * array_sizes_capacity);
      array_sizes = new_array;
    }
  }

  if (is_array) {
    // Build array declarators in source order so outer dimensions wrap last.
    for (size_t i = 0; i < num_array_sizes; i++) {
      struct ArrayDec array_dec = {decl, array_sizes[i]};
      struct Declarator* new_decl = arena_alloc(sizeof(struct Declarator));
      new_decl->type = ARRAY_DEC;
      new_decl->declarator.array_dec = array_dec;
      decl = new_decl;
    }
    free(array_sizes);
    return decl;
  }

  // simple declarator
  return decl;
}

struct ParamTypeList* params_to_types(struct ParamList* params){
  struct ParamTypeList* head = NULL;
  struct ParamTypeList* tail = head;
  for (struct ParamList* cur = params; cur != NULL; cur = cur->next){
    struct ParamTypeList* node = arena_alloc(sizeof(struct ParamTypeList));
    node->type = cur->param.type;
    node->next = NULL;
    if (head == NULL) {
      head = node;
      tail = node;
    } else {
      tail->next = node;
      tail = node;
    }
  }
  return head;
}

// Purpose: Convert a parameter declarator into a VariableDclr entry.
// Inputs: param describes the parameter; name_out/type_out capture results.
// Outputs: Returns true on success and updates output pointers.
// Invariants/Assumptions: Parameter declarators follow the same rules as variables.
static bool process_param_info(struct ParamInfo* param, struct Slice** name_out,
                               struct Type** type_out){
  struct ParamList* ignored_params = NULL;
  return process_declarator(&param->decl, param->type, name_out, type_out,
                            &ignored_params);
}

// Purpose: Convert a parsed parameter list into VariableDclr nodes.
// Inputs: params is the parsed ParamInfo list; params_out receives the result.
// Outputs: Returns true on success and builds a ParamList chain.
// Invariants/Assumptions: Parameter parsing order is preserved.
bool process_params_info(struct ParamInfoList* params, struct ParamList** params_out){
  if (params == NULL){
    *params_out = NULL;
    return true;
  }
  // Sentinel for empty parameter list produced by parse_params().
  if (params->info.type == NULL &&
      params->info.decl.type == IDENT_DEC &&
      params->info.decl.declarator.ident_dec.name == NULL){
    *params_out = NULL;
    return true;
  }
  struct ParamList* head = NULL;
  struct ParamList* tail = head;
  for (struct ParamInfoList* cur = params; cur != NULL; cur = cur->next){
    struct Slice* name = NULL;
    struct Type* type_ = NULL;
    if (!process_param_info(&cur->info, &name, &type_)){
      return false;
    }
    struct ParamList* node = arena_alloc(sizeof(struct ParamList));
    node->param.name = name;
    node->param.type = type_;
    node->param.init = NULL;
    node->param.storage = NONE;
    node->param.attributes.cleanup_func = NULL;
    node->next = NULL;
    if (head == NULL){
      head = node;
      tail = node;
    } else {
      tail->next = node;
      tail = node;
    }
  }
  *params_out = head;
  return true;
}

bool process_declarator(struct Declarator* decl, struct Type* base_type,
                        struct Slice** name_out, struct Type** derived_type_out,
                        struct ParamList** params_out){
  switch (decl->type){
    case IDENT_DEC:
      *name_out = decl->declarator.ident_dec.name;
      *derived_type_out = base_type;
      *params_out = NULL;
      return true;
    case POINTER_DEC: {
      // Build a pointer type and recurse inward for further derivations.
      struct Type* ptr_type = arena_alloc(sizeof(struct Type));
      ptr_type->type = POINTER_TYPE;
      ptr_type->type_data.pointer_type.referenced_type = base_type;
      return process_declarator(decl->declarator.pointer_dec.decl, ptr_type,
                                name_out, derived_type_out, params_out);
    }
    case FUN_DEC: {
      if (base_type->type == FUN_TYPE) {
        parse_error_at(parser_error_ptr(), "function cannot return function type");
        return false;
      }
      // Function declarators only allow identifiers at the core.
      struct Declarator* inner_decl = decl->declarator.fun_dec.decl;
      struct ParamList* params = NULL;
      if (!process_params_info(decl->declarator.fun_dec.params, &params)){
        return false;
      }
      struct Type* fun_type = arena_alloc(sizeof(struct Type));
      fun_type->type = FUN_TYPE;
      fun_type->type_data.fun_type.return_type = base_type;
      fun_type->type_data.fun_type.param_types = params_to_types(params);
      if (inner_decl->type == IDENT_DEC){
        *name_out = inner_decl->declarator.ident_dec.name;
        *derived_type_out = fun_type;
        *params_out = params;
        return true;
      }
      *params_out = NULL;
      return process_declarator(inner_decl, fun_type,
                                name_out, derived_type_out, params_out);
    }
    case ARRAY_DEC: {
      // Build an array type and recurse inward for further derivations

      struct Type* array_type = arena_alloc(sizeof(struct Type));
      array_type->type = ARRAY_TYPE;
      array_type->type_data.array_type.element_type = base_type;
      array_type->type_data.array_type.size = decl->declarator.array_dec.size;
      return process_declarator(decl->declarator.array_dec.decl, array_type,
                                name_out, derived_type_out, params_out);
    }
  }
  return false;
}

// Purpose: Build a placeholder block for an empty function body.
// Inputs: loc is the location to associate with the empty statement.
// Outputs: Returns a Block containing a NULL_STMT item.
// Invariants/Assumptions: Used only when parsing a function body with "{}".
static struct Block* make_empty_block(char* loc) {
  struct Statement* null_stmt = arena_alloc(sizeof(struct Statement));
  null_stmt->loc = loc;
  null_stmt->type = NULL_STMT;

  struct BlockItem* item = arena_alloc(sizeof(struct BlockItem));
  item->type = STMT_ITEM;
  item->item.stmt = null_stmt;

  struct Block* block = arena_alloc(sizeof(struct Block));
  block->item = item;
  block->next = NULL;
  return block;
}

// Purpose: Parse the trailing ';' or function body after a declarator.
// Inputs: success is set false on parse failure.
// Outputs: Returns a Block pointer for a function body or NULL for a prototype.
// Invariants/Assumptions: Caller handles function declaration construction.
struct Block* parse_end_of_func(bool* success){
  bool success2;
  struct Block* body = parse_block(&success2);
  if (success2){
    *success = true;
    if (body == NULL) {
      return make_empty_block(NULL);
    }
    return body;
  }
  if (consume(SEMI)){
    *success = true;
    return NULL;
  }
  *success = false;
  return NULL;
}

// Purpose: Parse a function declaration or definition.
// Inputs: ret_type/storage come from earlier specifier parsing.
// Outputs: Returns a FunctionDclr node or NULL on failure.
// Invariants/Assumptions: Declarator parsing determines function name and params.
struct FunctionDclr* parse_function(struct Type* ret_type, enum StorageClass storage, 
                                    struct Slice* name, struct ParamList* params){
  struct Type* fun_type = arena_alloc(sizeof(struct Type));
  fun_type->type = FUN_TYPE;
  fun_type->type_data.fun_type.return_type = ret_type;
  fun_type->type_data.fun_type.param_types = params_to_types(params);
  struct FunctionDclr* result = arena_alloc(sizeof(struct FunctionDclr));
  result->name = name;
  result->params = params;
  result->storage = storage;
  result->type = fun_type;
  bool success;
  result->body = parse_end_of_func(&success);
  if (!success){
    return NULL;
  }
  return result;
}

struct VarAttributes* parse_var_attributes(){
  // only supports __attribute__((cleanup(...))) for now
  struct VarAttributes* attrs = arena_alloc(sizeof(struct VarAttributes));
  if (!consume(ATTRIBUTE_TOK)){
    attrs->cleanup_func = NULL;
    return attrs;
  }

  // there is an attribute list
  if (!consume(OPEN_P) || !consume(OPEN_P)){
    parse_error_at(parser_error_ptr(), "expected '((' after 'attribute'");
    return NULL;
  }

  union TokenVariant* data = consume_with_data(IDENT);
  if (data == NULL){
    parse_error_at(parser_error_ptr(), "expected identifier for attribute");
    return NULL;
  }
  if (!compare_slice_to_pointer(data->ident_name, "cleanup")){
    parse_error_at(parser_error_ptr(), "only 'cleanup' attribute is supported");
    return NULL;
  }
  if (!consume(OPEN_P)){
    parse_error_at(parser_error_ptr(), "expected '(' after 'cleanup'");
    return NULL;
  }
  data = consume_with_data(IDENT);
  if (data == NULL){
    parse_error_at(parser_error_ptr(), "expected identifier for cleanup function");
    return NULL;
  }
  attrs->cleanup_func = data->ident_name;

  if (!consume(CLOSE_P) || !consume(CLOSE_P) || !consume(CLOSE_P)){
    parse_error_at(parser_error_ptr(), "expected ')))' after attribute identifier");
    return NULL;
  }

  return attrs;
}

struct MemberDclr* parse_member_declarations(){
  struct MemberDclr* head = NULL;
  struct MemberDclr** tail = &head;
  while (true){
    struct Type* base_type = NULL;
    enum StorageClass storage = NONE;
    parse_type_and_storage_class(&base_type, &storage);
    if (base_type == NULL) break;

    struct Declarator* declarator = parse_declarator();
    if (declarator == NULL){
      parse_error_at(parser_error_ptr(), "expected member declarator");
      return NULL;
    }

    struct Slice* name = NULL;
    struct Type* decl_type = NULL;
    struct ParamList* params = NULL;
    if (!process_declarator(declarator, base_type, &name, &decl_type, &params)){
      parse_error_at(parser_error_ptr(), "failed to process member declarator");
      return NULL;
    }
    if (decl_type->type == FUN_TYPE){
      parse_error_at(parser_error_ptr(), "member cannot be a function");
      return NULL;
    }

    if (!consume(SEMI)){
      parse_error_at(parser_error_ptr(), "expected ';' after member declaration");
      return NULL;
    }

    struct MemberDclr* member = arena_alloc(sizeof(struct MemberDclr));
    member->name = name;
    member->type = decl_type;
    member->next = NULL;
    *tail = member;
    tail = &member->next;
  }
  return head;
}

struct EnumMemberDclr* parse_enumerator_list(){
  struct EnumMemberDclr* head = NULL;
  struct EnumMemberDclr** tail = &head;
  unsigned enum_value = 0;
  while (true){
    union TokenVariant* data = consume_with_data(IDENT);
    if (data == NULL){
      break;
    }
    if (consume(EQUALS)){
      struct LitExpr value_expr = parse_lit_expr();
      if (value_expr.type == -1){
        parse_error_at(parser_error_ptr(), "expected constant expression for enumerator value");
        return NULL;
      }
      if (value_expr.value.uint_val < enum_value){
        parse_error_at(parser_error_ptr(), "enumerator value must be increasing");
        return NULL;
      }
      enum_value = value_expr.value.uint_val;
    }
    struct EnumMemberDclr* enumerator = arena_alloc(sizeof(struct EnumMemberDclr));
    enumerator->name = data->ident_name;
    enumerator->value = enum_value;
    enumerator->next = NULL;
    *tail = enumerator;
    tail = &enumerator->next;
    enum_value++;

    if (consume(COMMA)) continue;
    break;
  }
  return head;
}

// Parse a full declaration (function or variable).
// Purpose: Parse a declaration (function or variable) at any scope.
// Inputs: Consumes tokens from the current cursor.
// Outputs: Returns a Declaration node or NULL on failure.
// Invariants/Assumptions: Storage class and type specifiers precede declarators.
struct Declaration* parse_declaration(){
  struct Token* old_current = current;

  struct Declaration* result = arena_alloc(sizeof(struct Declaration));

  // check for struct, union, or enum type declaration
  if (consume(STRUCT_TOK)) {
    union TokenVariant* data = consume_with_data(IDENT);
    if (data == NULL){
      parse_error_at(parser_error_ptr(), "expected identifier after 'struct'");
      return NULL;
    }
    result->type = STRUCT_DCLR;
    result->dclr.struct_dclr.name = data->ident_name;
    result->dclr.struct_dclr.members = NULL;

    if (consume(OPEN_B)){
      // parse member declarations
      struct MemberDclr* members = parse_member_declarations();
      if (members == NULL){
        parse_error_at(parser_error_ptr(), "expected at least one struct member declaration");
        return NULL;
      }
      result->dclr.struct_dclr.members = members;

      if (!consume(CLOSE_B)){
        // for now, require at least one member
        parse_error_at(parser_error_ptr(), "expected '}' after struct member declarations");
        return NULL;
      }
    }

    if (!consume(SEMI)){
      // may be a variable declaration of struct type, so don't error yet
      current = old_current;
    } else {
      // type declaration successfully parsed
      return result;
    }
  } else if (consume(UNION_TOK)) {
    union TokenVariant* data = consume_with_data(IDENT);
    if (data == NULL){
      parse_error_at(parser_error_ptr(), "expected identifier after 'union'");
      return NULL;
    }
    result->type = UNION_DCLR;
    result->dclr.union_dclr.name = data->ident_name;
    result->dclr.union_dclr.members = NULL;

    if (consume(OPEN_B)){
      // parse member declarations
      struct MemberDclr* members = parse_member_declarations();
      if (members == NULL){
        parse_error_at(parser_error_ptr(), "expected at least one union member declaration");
        return NULL;
      }
      result->dclr.union_dclr.members = members;
      if (!consume(CLOSE_B)){
        // for now, require at least one member
        parse_error_at(parser_error_ptr(), "expected '}' after union member declarations");
        return NULL;
      }
    }

    if (!consume(SEMI)){
      // may be a variable declaration of union type, so don't error yet
      current = old_current;
    } else {
      // type declaration successfully parsed
      return result;
    }
  } else if (consume(ENUM_TOK)) {
    union TokenVariant* data = consume_with_data(IDENT);
    if (data == NULL){
      parse_error_at(parser_error_ptr(), "expected identifier after 'enum'");
      return NULL;
    }
    result->type = ENUM_DCLR;
    result->dclr.enum_dclr.name = data->ident_name;
    result->dclr.enum_dclr.members = NULL;

    if (consume(OPEN_B)){
      // require definition for enum

      // parse enumerator list
      struct EnumMemberDclr* enumerators = parse_enumerator_list();
      if (enumerators == NULL){
        parse_error_at(parser_error_ptr(), "expected at least one enumerator");
        return NULL;
      }
      result->dclr.enum_dclr.members = enumerators;
      if (!consume(CLOSE_B)){
        // for now, require at least one enumerator
        parse_error_at(parser_error_ptr(), "expected '}' after enumerator list");
        return NULL;
      }

      if (!consume(SEMI)){
        // may be a variable declaration of enum type, so don't error yet
        current = old_current;
      } else {
        // type declaration successfully parsed
        return result;
      } 
    } else {
      current = old_current;
    }
  }

  struct VarAttributes* attrs = NULL;
  if ((attrs = parse_var_attributes()) == NULL) return NULL; // possible attribute location 1
  if (token_index(current) >= prog_size) return NULL;
  if (!is_type_specifier(current->type)) return NULL;
  
  struct Type* base_type = NULL;
  enum StorageClass storage = NONE;
  parse_type_and_storage_class(&base_type, &storage);
  if (base_type == NULL){
    current = old_current;
    return NULL;
  }
  struct VarAttributes* new_attrs = NULL;
  if ((new_attrs = parse_var_attributes()) == NULL) return NULL; // possible attribute location 2
  if (attrs->cleanup_func != NULL && new_attrs->cleanup_func != NULL){
    parse_error_at(parser_error_ptr(), "cleanup specified multiple times");
    return NULL;
  }
  if (new_attrs->cleanup_func != NULL){
    attrs = new_attrs;
  }

  struct Declarator* declarator = parse_declarator();
  if (declarator == NULL){
    parse_error_at(parser_error_ptr(), "expected declarator after type specifiers");
    return NULL;
  }
  if ((new_attrs = parse_var_attributes()) == NULL) return NULL; // possible attribute location 3
  if (attrs->cleanup_func != NULL && new_attrs->cleanup_func != NULL){
    parse_error_at(parser_error_ptr(), "cleanup specified multiple times");
    return NULL;
  }
  if (new_attrs->cleanup_func != NULL){
    attrs = new_attrs;
  }

  struct Slice* name = NULL;
  struct Type* decl_type = NULL;
  struct ParamList* params = NULL;
  if (!process_declarator(declarator, base_type, &name, &decl_type, &params)){
    current = old_current;
    return NULL;
  }

  if (decl_type->type == FUN_TYPE){
    // function declaration
    struct Type* ret_type = decl_type->type_data.fun_type.return_type;
    struct FunctionDclr* fun_dclr = parse_function(ret_type, storage, name, params);
    if (fun_dclr == NULL){
      current = old_current;
      return NULL;
    }
    result->type = FUN_DCLR;
    result->dclr.fun_dclr = *fun_dclr;
    return result;
  }

  // variable declaration
  struct VariableDclr* var_dclr = parse_var_dclr(decl_type, storage, name);
  if (var_dclr == NULL || !consume(SEMI)){
    current = old_current;
    return NULL;
  }
  result->type = VAR_DCLR;
  result->dclr.var_dclr = *var_dclr;
  result->dclr.var_dclr.attributes = *attrs;
  return result;
}

// Top-level parser entry; consumes all declarations in the token stream.
// Purpose: Parse the entire token array into a Program node.
// Inputs: arr is the token array produced by the lexer.
// Outputs: Returns a Program node or NULL on failure.
// Invariants/Assumptions: Parsing errors are reported via print_error.
struct Program* parse_prog(struct TokenArray* arr){
  if (arena == NULL) {
    fdputs(STDERR, "Parser requires an arena\n");
    return NULL;
  }

  // parse declarations and put them in a linked list
  program = arr->tokens;
  current = program;
  prog_size = arr->size;
  max_consumed_valid = false;
  max_consumed_index = 0;

  struct Program* prog = arena_alloc(sizeof(struct Program));
  struct Declaration* dclr = parse_declaration();

  if (dclr == NULL) {
    if (token_index(current) < prog_size) {
      print_error();
      return NULL;
    }
    // there were 0 declarations
    prog->dclrs = NULL;
    return prog;
  }

  struct DeclarationList* head = arena_alloc(sizeof(struct DeclarationList));
  head->dclr = *dclr;
  head->next = NULL;
  prog->dclrs = head;

  struct DeclarationList* tail = head;
  while ((dclr = parse_declaration()) != NULL){
    struct DeclarationList* next_dclr = arena_alloc(sizeof(struct DeclarationList));
    next_dclr->dclr = *dclr;
    next_dclr->next = NULL;
    tail->next = next_dclr;
    tail = next_dclr;
  }

  // ensure the entire token array has been consumed
  if (token_index(current) != prog_size) {
    print_error();
    return NULL;
  }

  return prog;
}
