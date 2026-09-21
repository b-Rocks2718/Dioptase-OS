#ifndef PARSER_H
#define PARSER_H

#include "AST.h"
#include "token_array.h"

#include "../crt/stdbool.h"

struct Arena;

// Declare parser entry points for the C subset.
// Return AST nodes or NULL on failure.

// Parse an entire token array into a Program AST.
// Returns a Program or NULL if parsing fails.
// Tokens must remain valid for the parse duration.
struct Program* parse_prog(struct TokenArray* tokens);

struct Block* parse_block(bool* success);

struct VarAttributes* parse_var_attributes(void);

// Parse a single statement from the current token cursor.
// Returns a Statement node or NULL if no statement matches.
struct Statement* parse_statement();

// Parse an expression with full precedence handling, including comma operator
// Returns an Expr node or NULL on failure.
// Cursor points to the first token of an expression.
struct Expr* parse_expr();

// Parse an assignment expression (no comma operator).
// Returns an Expr node or NULL on failure.
// Cursor points to the first token of an expression.
struct Expr* parse_assignment_expr();

// Parse unary expressions including prefix operators and casts.
// Returns an Expr node or NULL if no unary expression matches.
// Cursor points to a unary-expression start.
struct Expr* parse_unary();

// Parse a binary expression using precedence rules.
// Returns an Expr node or NULL on failure.
struct Expr* parse_bin_expr(unsigned min_prec);

// Parse the lowest-precedence expression forms.
// Returns an Expr node or NULL on failure.
struct Expr* parse_primary_expr();

struct Expr* parse_factor();

// factor but without casts
// used because sizeof accepts this as an argument, but not casts
struct Expr* parse_sub_factor();

// Parse a local type for sizeof(type) and casts.
// Returns a Type node or NULL on failure.
struct Type* parse_local_type();

// Parse a variable expression.
// Returns a VAR expression or NULL if no identifier matches.
// Cursor points at an identifier token.
struct Expr* parse_var();

// Parse an abstract declarator for casts and type names.
// Returns an AbstractDeclarator or NULL if absent.
struct AbstractDeclarator* parse_abstract_declarator();

// Parse a variable declaration after a declarator is known.
// Returns a VariableDclr node or NULL on failure.
struct VariableDclr* parse_var_dclr(struct Type* type, enum StorageClass storage,
                                    struct Slice* name);

// Parse a full declaration (variable or function).
// Returns a Declaration or NULL when no declaration matches.
// Cursor points at a declaration start.
struct Declaration* parse_declaration();

// Parse a declarator (possibly with pointers and parameters).
// Returns a Declarator node or NULL on failure.
struct Declarator* parse_declarator();

// Parse the non-parameter portion of a declarator.
// Returns a Declarator node or NULL when not present.
struct Declarator* parse_simple_declarator();

// Parse a direct declarator (identifier or parenthesized declarator).
// Returns a Declarator node or NULL on failure.
struct Declarator* parse_direct_declarator();

// Apply declarator structure to a base type, producing name/type/params.
// Walks the declarator tree and constructs the derived type in order.
// Convert a declarator into a finalized type and name.
// Returns true on success and populates type_out/name_out.
bool process_declarator(struct Declarator* decl, struct Type* base_type,
                        struct Slice** name_out, struct Type** derived_type_out,
                        struct ParamList** params_out);

// Parse a token stream into AST structures for the C subset.
// Allocates AST nodes in the arena and returns parse results.

void parse_type_and_storage_class(struct Type** type, enum StorageClass* class);

struct LitExpr parse_lit_expr(void);

#endif
