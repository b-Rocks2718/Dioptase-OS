#ifndef IDENTIFIER_RESOLUTION_H
#define IDENTIFIER_RESOLUTION_H

#include "../crt/stdbool.h"
#include "arena.h"
#include "AST.h"

// Purpose: Resolve identifiers to unique names across scopes.
// Inputs: Traverses AST nodes produced by the parser.
// Outputs: Rewrites identifier slices for locals and validates declarations.
// Invariants/Assumptions: Uses a scoped identifier map stored in arena memory.

// Purpose: Resolve identifiers inside an expression subtree.
// Inputs: expr is the expression to analyze.
// Outputs: Returns true on success; false on unresolved identifiers.
// Invariants/Assumptions: Identifier map is initialized before traversal.
bool resolve_expr(struct Expr* expr);

// Purpose: Resolve identifiers in a local-scope declaration.
// Inputs: dclr is the declaration node.
// Outputs: Returns true on success; false on redeclaration errors.
// Invariants/Assumptions: Caller manages scope entry/exit.
bool resolve_local_dclr(struct Declaration* dclr);

// Purpose: Resolve identifiers in a file-scope declaration.
// Inputs: dclr is the declaration node.
// Outputs: Returns true on success; false on invalid file-scope redeclarations.
// Invariants/Assumptions: File-scope entries share one global scope.
bool resolve_file_scope_dclr(struct Declaration* dclr);

// Purpose: Resolve identifiers in a local variable declaration.
// Inputs: var_dclr is the variable declaration node.
// Outputs: Returns true on success; false on redeclaration conflicts.
// Invariants/Assumptions: Locals may be renamed to unique slices.
bool resolve_local_var_dclr(struct VariableDclr* var_dclr);

bool resolve_struct(struct StructDclr* struct_dclr);

bool resolve_union(struct UnionDclr* union_dclr);

bool resolve_enum(struct EnumDclr* enum_dclr);

bool resolve_var_init(struct Initializer* init);

// Purpose: Resolve identifiers in a file-scope variable declaration.
// Inputs: var_dclr is the variable declaration node.
// Outputs: Returns true on success; false on invalid file-scope redeclarations.
// Invariants/Assumptions: File-scope variables retain their original names.
bool resolve_file_scope_var_dclr(struct VariableDclr* var_dclr);

// Purpose: Resolve identifiers in a file-scope function declaration or definition.
// Inputs: func_dclr is the function declaration node.
// Outputs: Returns true on success; false on invalid redeclarations.
// Invariants/Assumptions: Function bodies introduce a new scope for params/locals.
bool resolve_file_scope_func(struct FunctionDclr* func_dclr);

// Purpose: Resolve identifiers in a local (block-scope) function declaration.
// Inputs: func_dclr is the function declaration node.
// Outputs: Returns true on success; false on invalid linkage or bodies.
// Invariants/Assumptions: Local functions must be extern-only declarations.
bool resolve_local_func(struct FunctionDclr* func_dclr);

// Purpose: Resolve identifiers within a block item list.
// Inputs: block is the block list.
// Outputs: Returns true on success; false on any resolution error.
// Invariants/Assumptions: Caller manages scope entry/exit as needed.
bool resolve_block(struct Block* block);

bool resolve_type(struct Type* type);

// Purpose: Resolve identifiers for all declarations in a program.
// Inputs: prog is the top-level Program node.
// Outputs: Returns true on success; false on any resolution error.
// Invariants/Assumptions: Initializes and destroys the global identifier stack.
bool resolve_prog(struct Program* prog);

#endif // IDENTIFIER_RESOLUTION_H
