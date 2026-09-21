#ifndef IDENTIFIER_RESOLUTION_H
#define IDENTIFIER_RESOLUTION_H

#include "../crt/stdbool.h"
#include "arena.h"
#include "AST.h"

// Resolve identifiers to unique names across scopes.
// Rewrites identifier slices for locals and validates declarations.

// Resolve identifiers inside an expression subtree.
// Returns true on success; false on unresolved identifiers.
// Identifier map is initialized before traversal.
bool resolve_expr(struct Expr* expr);

// Resolve identifiers in a local-scope declaration.
// Returns true on success; false on redeclaration errors.
bool resolve_local_dclr(struct Declaration* dclr);

// Resolve identifiers in a file-scope declaration.
// Returns true on success; false on invalid file-scope redeclarations.
bool resolve_file_scope_dclr(struct Declaration* dclr);

// Resolve identifiers in a local variable declaration.
// Returns true on success; false on redeclaration conflicts.
// Locals may be renamed to unique slices.
bool resolve_local_var_dclr(struct VariableDclr* var_dclr);

bool resolve_struct(struct StructDclr* struct_dclr);

bool resolve_union(struct UnionDclr* union_dclr);

bool resolve_enum(struct EnumDclr* enum_dclr);

bool resolve_var_init(struct Initializer* init);

// Resolve identifiers in a file-scope variable declaration.
// Returns true on success; false on invalid file-scope redeclarations.
bool resolve_file_scope_var_dclr(struct VariableDclr* var_dclr);

// Resolve identifiers in a file-scope function declaration or definition.
// Returns true on success; false on invalid redeclarations.
bool resolve_file_scope_func(struct FunctionDclr* func_dclr);

// Resolve identifiers in a local (block-scope) function declaration.
// Returns true on success; false on invalid linkage or bodies.
// Local functions must be extern-only declarations.
bool resolve_local_func(struct FunctionDclr* func_dclr);

// Resolve identifiers within a block item list.
// Returns true on success; false on any resolution error.
bool resolve_block(struct Block* block);

bool resolve_type(struct Type* type);

// Resolve identifiers for all declarations in a program.
// Returns true on success; false on any resolution error.
bool resolve_prog(struct Program* prog);

#endif // IDENTIFIER_RESOLUTION_H
