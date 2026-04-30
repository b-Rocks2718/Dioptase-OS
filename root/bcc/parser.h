#ifndef PARSER_H
#define PARSER_H

#include "AST.h"
#include "token_array.h"

#include "../crt/stdbool.h"

struct Arena;

// Purpose: Declare parser entry points for the C subset.
// Inputs: Functions consume parser state seeded by parse_prog.
// Outputs: Return AST nodes or NULL on failure.
// Invariants/Assumptions: Parsing allocates from the arena and uses token locations.

// Purpose: Parse an entire token array into a Program AST.
// Inputs: tokens is the lexer output for a translation unit.
// Outputs: Returns a Program or NULL if parsing fails.
// Invariants/Assumptions: tokens must remain valid for the parse duration.
struct Program* parse_prog(struct TokenArray* tokens);

struct Block* parse_block(bool* success);

struct VarAttributes* parse_var_attributes(void);

// Purpose: Parse a single statement from the current token cursor.
// Inputs: Consumes the global parser token stream.
// Outputs: Returns a Statement node or NULL if no statement matches.
// Invariants/Assumptions: The parser cursor is positioned at a statement start.
struct Statement* parse_statement();

// Purpose: Parse an expression with full precedence handling, including comma operator
// Inputs: Consumes the global parser token stream.
// Outputs: Returns an Expr node or NULL on failure.
// Invariants/Assumptions: Cursor points to the first token of an expression.
struct Expr* parse_expr();

// Purpose: Parse an assignment expression (no comma operator).
// Inputs: Consumes the global parser token stream.
// Outputs: Returns an Expr node or NULL on failure.
// Invariants/Assumptions: Cursor points to the first token of an expression.
struct Expr* parse_assignment_expr();

// Purpose: Parse unary expressions including prefix operators and casts.
// Inputs: Consumes the global parser token stream.
// Outputs: Returns an Expr node or NULL if no unary expression matches.
// Invariants/Assumptions: Cursor points to a unary-expression start.
struct Expr* parse_unary();

// Purpose: Parse a binary expression using precedence rules.
// Inputs: Consumes the global parser token stream.
// Outputs: Returns an Expr node or NULL on failure.
// Invariants/Assumptions: parse_unary/parse_factor handle lower-level forms.
struct Expr* parse_bin_expr(unsigned min_prec);

// Purpose: Parse the lowest-precedence expression forms.
// Inputs: Consumes the global parser token stream.
// Outputs: Returns an Expr node or NULL on failure.
// Invariants/Assumptions: Factors include literals, variables, calls, and parenthesized forms.
struct Expr* parse_primary_expr();

struct Expr* parse_factor();

// factor but without casts
// used because sizeof accepts this as an argument, but not casts
struct Expr* parse_sub_factor();

// Purpose: Parse a local type for sizeof(type) and casts.
// Inputs: Consumes the global parser token stream.
// Outputs: Returns a Type node or NULL on failure.
// Invariants/Assumptions: Caller has consumed the opening '(' and will consume closing ')'.
struct Type* parse_local_type();

// Purpose: Parse a variable expression.
// Inputs: Consumes the global parser token stream.
// Outputs: Returns a VAR expression or NULL if no identifier matches.
// Invariants/Assumptions: Cursor points at an identifier token.
struct Expr* parse_var();

// Purpose: Parse an abstract declarator for casts and type names.
// Inputs: Consumes the global parser token stream.
// Outputs: Returns an AbstractDeclarator or NULL if absent.
// Invariants/Assumptions: Caller handles the surrounding type specifiers.
struct AbstractDeclarator* parse_abstract_declarator();

// Purpose: Parse a variable declaration after a declarator is known.
// Inputs: Consumes the global parser token stream.
// Outputs: Returns a VariableDclr node or NULL on failure.
// Invariants/Assumptions: Caller supplies the type and identifier context.
struct VariableDclr* parse_var_dclr(struct Type* type, enum StorageClass storage,
                                    struct Slice* name);

// Purpose: Parse a full declaration (variable or function).
// Inputs: Consumes the global parser token stream.
// Outputs: Returns a Declaration or NULL when no declaration matches.
// Invariants/Assumptions: Cursor points at a declaration start.
struct Declaration* parse_declaration();

// Purpose: Parse a declarator (possibly with pointers and parameters).
// Inputs: Consumes the global parser token stream.
// Outputs: Returns a Declarator node or NULL on failure.
// Invariants/Assumptions: Caller supplies base type specifiers separately.
struct Declarator* parse_declarator();

// Purpose: Parse the non-parameter portion of a declarator.
// Inputs: Consumes the global parser token stream.
// Outputs: Returns a Declarator node or NULL when not present.
// Invariants/Assumptions: Used for parameter and typedef contexts.
struct Declarator* parse_simple_declarator();

// Purpose: Parse a direct declarator (identifier or parenthesized declarator).
// Inputs: Consumes the global parser token stream.
// Outputs: Returns a Declarator node or NULL on failure.
// Invariants/Assumptions: Pointer prefixes are handled by parse_declarator.
struct Declarator* parse_direct_declarator();

// Apply declarator structure to a base type, producing name/type/params.
// Walks the declarator tree and constructs the derived type in order.
// Purpose: Convert a declarator into a finalized type and name.
// Inputs: decl is the parsed declarator; base_type is the starting type.
// Outputs: Returns true on success and populates type_out/name_out.
// Invariants/Assumptions: decl structure is validated by the parser.
bool process_declarator(struct Declarator* decl, struct Type* base_type,
                        struct Slice** name_out, struct Type** derived_type_out,
                        struct ParamList** params_out);

// Purpose: Parse a token stream into AST structures for the C subset.
// Inputs: Consumes tokens produced by the lexer.
// Outputs: Allocates AST nodes in the arena and returns parse results.
// Invariants/Assumptions: Tokens are well-formed and include locations.

void parse_type_and_storage_class(struct Type** type, enum StorageClass* class);

struct LitExpr parse_lit_expr(void);

#endif
