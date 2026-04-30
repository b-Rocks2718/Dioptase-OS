#ifndef TYPECHECKING_H
#define TYPECHECKING_H

#include "AST.h"
#include "slice.h"

#include "../crt/stdbool.h"
#include "../crt/stdint.h"

// Purpose: Define typechecking data structures and APIs.
// Inputs: Operates on AST nodes and slices from parsing/resolution.
// Outputs: Provides typechecking results and symbol table accessors.
// Invariants/Assumptions: Types and identifiers are arena-allocated.

struct IdentAttr;
struct InitList;

// Purpose: Entry in the symbol table for a single identifier.
// Inputs: key is the identifier name; type and attrs describe semantics.
// Outputs: Used by typechecker to validate uses and linkage.
// Invariants/Assumptions: key/type/attrs pointers remain valid for pass lifetime.
struct SymbolEntry{
  struct Slice* key;
  struct Type* type;
  struct IdentAttr* attrs;
  struct SymbolEntry* next;
};

enum TypeEntryType {
  STRUCT_ENTRY,
  UNION_ENTRY,
  ENUM_ENTRY,
};

struct MemberEntry{
  struct Slice* key;
  struct Type* type;
  size_t offset;
  struct MemberEntry* next;
};

struct StructEntry{
  struct Slice* key;
  unsigned alignment;
  unsigned size;
  struct MemberEntry* members;
};

union TypeEntryVariant {
  struct StructEntry* struct_entry;
  struct StructEntry* union_entry;
};

struct TypeEntry{
  struct Slice* key;
  enum TypeEntryType type;
  union TypeEntryVariant data;
  struct TypeEntry* next;
};

// Purpose: Hash table mapping type names to type entries.
// Inputs: size is bucket count; arr holds bucket chains.
// Outputs: Used as a type table in typechecking.
// Invariants/Assumptions: size is non-zero; arr length equals size.
struct TypeTable{
  size_t size;
  struct TypeEntry** arr;
};

// Purpose: Hash table mapping identifier names to symbol entries.
// Inputs: size is bucket count; arr holds bucket chains.
// Outputs: Used as the global symbol table in typechecking.
// Invariants/Assumptions: size is non-zero; arr length equals size.
struct SymbolTable{
	size_t size;
  struct SymbolEntry** arr;
};

// Purpose: Describe the category of identifier in the symbol table.
// Inputs: Stored in IdentAttr.attr_type.
// Outputs: Guides typechecking and linkage rules.
// Invariants/Assumptions: FUN_ATTR denotes functions; STATIC_ATTR/LOCAL_ATTR denote vars.
enum IdentAttrType {
  FUN_ATTR,
  STATIC_ATTR,
  LOCAL_ATTR,
  CONST_ATTR,
};

// Purpose: Track initialization state for static storage.
// Inputs: Stored in IdentInit.init_type.
// Outputs: Guides tentative definition and redefinition rules.
// Invariants/Assumptions: Higher enum values represent more-defined states.
enum IdentInitType {
  NO_INIT = 0,
  TENTATIVE = 1,
  INITIAL = 2
};

// Purpose: Describe the initializer state for a static or global variable.
// Inputs: init_type indicates the initializer state; init_list holds data.
// Outputs: Stored in IdentAttr for later codegen.
// Invariants/Assumptions: init_list is valid only when init_type == INITIAL.
struct IdentInit {
  enum IdentInitType init_type;
  struct InitList* init_list; // valid if init_type == INITIAL
};

// Purpose: Enumerate the kinds of static initialization supported.
// Inputs: Stored in InitValue.int_type.
// Outputs: Guides data emission for static variables.
// Invariants/Assumptions: ZERO_INIT represents zero-fill.
enum StaticInitType {
  CHAR_INIT,
  UCHAR_INIT,
  SHORT_INIT,
  USHORT_INIT,
  INT_INIT,
  UINT_INIT,
  LONG_INIT,
  ULONG_INIT,
  STRING_INIT,
  POINTER_INIT,
  ZERO_INIT,
};

union StaticInitValue {
  uint64_t num;
  struct Slice* string; // for STRING_INIT
  struct Slice* pointer;     // for POINTER_INIT
};

// Purpose: Hold the value for a static initializer entry.
// Inputs: int_type selects the kind; value holds the normalized bits.
// Outputs: Used by InitList to describe static data.
// Invariants/Assumptions: value is normalized to the initializer type width.
struct StaticInit {
  enum StaticInitType int_type;
  union StaticInitValue value;
};

// Purpose: Singly linked list of static initializer values.
// Inputs: value holds the initializer data; next links additional entries.
// Outputs: Stored in IdentInit for static variable initialization.
// Invariants/Assumptions: List order matches declaration order.
struct InitList {
  struct StaticInit* value;
  struct InitList* next;
};

// Purpose: Attributes associated with a symbol table entry.
// Inputs: attr_type identifies kind; init tracks initialization.
// Outputs: Used by typechecking and later codegen phases.
// Invariants/Assumptions: is_defined indicates a definition has been seen.
struct IdentAttr {
  enum IdentAttrType attr_type;
  bool is_defined;
  enum StorageClass storage;
  struct IdentInit init;
  struct Slice* cleanup_handler;
};

// Purpose: Global symbol table for the active typechecking pass.
// Inputs: Initialized by typecheck_program.
// Outputs: Used for symbol lookup across the translation unit.
// Invariants/Assumptions: Only one typechecking pass runs at a time.
extern struct SymbolTable* global_symbol_table;
extern struct TypeTable* global_type_table;
extern struct Type kCharType;

// ------------------------- Typechecking Functions ------------------------- //

// Purpose: Typecheck every declaration in a program.
// Inputs: program is the Program AST.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Initializes global_symbol_table.
bool typecheck_program(struct Program* program);

// Purpose: Typecheck a file-scope declaration.
// Inputs: dclr is the declaration node.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: File-scope symbols live in global_symbol_table.
bool typecheck_file_scope_dclr(struct Declaration* dclr);

// Purpose: Typecheck a file-scope variable declaration/definition.
// Inputs: var_dclr is the variable declaration node.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Initializers must be literal constants for globals.
bool typecheck_file_scope_var(struct VariableDclr* var_dclr);

// Purpose: Typecheck a function declaration or definition.
// Inputs: func_dclr is the function declaration node.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Parameters and body share the global symbol table.
bool typecheck_func(struct FunctionDclr* func_dclr);

bool typecheck_struct(struct StructDclr* struct_dclr);

bool typecheck_union(struct UnionDclr* union_dclr);

bool typecheck_enum(struct EnumDclr* enum_dclr);

// Purpose: Typecheck and convert an initializer to a target type.
// Inputs: init is the initializer expression pointer; type is the target type.
// Outputs: Returns true on success; false on any conversion/type errors.
// Invariants/Assumptions: May rewrite *init with a cast expression.
bool typecheck_init(struct Initializer* init, struct Type* type);

// Purpose: Typecheck an expression subtree and assign value_type.
// Inputs: expr is the expression node.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: value_type is set for each expression.
bool typecheck_expr(struct Expr* expr);

// Purpose: Typecheck an expression with conversion rules.
// Inputs: expr is the expression node.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Currently delegates to typecheck_expr.
bool typecheck_convert_expr(struct Expr** expr);

// Purpose: Typecheck parameter declarations for a function.
// Inputs: params is the parameter list.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Parameters are inserted into the symbol table.
bool typecheck_params(struct ParamList* params);

// Purpose: Typecheck each item in a block.
// Inputs: block is the block list.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Uses the current symbol table for lookups.
bool typecheck_block(struct Block* block);

// Purpose: Typecheck a local declaration (variable or function).
// Inputs: dclr is the declaration node.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Local declarations share the global symbol table.
bool typecheck_local_dclr(struct Declaration* dclr);

// Purpose: Typecheck a statement subtree.
// Inputs: stmt is the statement node.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Return statements reference the enclosing function.
bool typecheck_stmt(struct Statement* stmt);

// Purpose: Typecheck a local variable declaration/definition.
// Inputs: var_dclr is the variable declaration node.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Local extern/static rules are enforced.
bool typecheck_local_var(struct VariableDclr* var_dclr);

// Purpose: Typecheck the initializer portion of a for loop.
// Inputs: init_ is the ForInit node.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: For-init may be a declaration or expression.
bool typecheck_for_init(struct ForInit* init_);

// Purpose: Typecheck a function call argument list.
// Inputs: args are call arguments; params are parameter types; call_site is for errors.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Arguments are converted by assignment.
bool typecheck_args(struct ArgList* args, struct ParamTypeList* params, struct Expr* call_site);

// ------------------------- Type Utility Functions ------------------------- //

// Purpose: Check whether a type is an arithmetic type.
// Inputs: type is the Type node.
// Outputs: Returns true for integer-like types.
// Invariants/Assumptions: Pointer and function types are not arithmetic.
bool is_arithmetic_type(struct Type* type);

bool is_char_type(struct Type* type);

// Purpose: Check whether a type is signed.
// Inputs: type is the Type node.
// Outputs: Returns true for signed integer types.
// Invariants/Assumptions: Unsigned types return false.
bool is_signed_type(struct Type* type);

// Purpose: Check whether a type is unsigned.
// Inputs: type is the Type node.
// Outputs: Returns true for unsigned integer types.
// Invariants/Assumptions: Signed types return false.
bool is_unsigned_type(struct Type* type);

// Purpose: Check whether a type is a pointer type.
// Inputs: type is the Type node.
// Outputs: Returns true if type->type == POINTER_TYPE.
// Invariants/Assumptions: Does not inspect referenced type.
bool is_pointer_type(struct Type* type);

bool is_void_pointer_type(struct Type* type);

bool is_complete_type(struct Type* type);

bool is_pointer_to_complete_type(struct Type* type);

bool is_valid_type_specifier(struct Type* type);

// scalar types include arithmetic types and pointer types
// do not include arrays, functions, or void
bool is_scalar_type(struct Type* type);

// Purpose: Check whether an expression is a null pointer constant.
// Inputs: expr is the expression node.
// Outputs: Returns true if expr is a null pointer constant.
// Invariants/Assumptions: Only integer literal 0 counts as null pointer constant.
bool is_null_pointer_constant(struct Expr* expr);

// Purpose: Apply assignment conversions to an expression.
// Inputs: expr is the expression pointer; target_type is the desired type.
// Outputs: Returns true on success; false on invalid conversions.
// Invariants/Assumptions: May rewrite *expr with a cast node.
bool convert_by_assignment(struct Expr** expr, struct Type* target_type);

// Purpose: Compute the common arithmetic type of two types.
// Inputs: type1 and type2 are the operand types.
// Outputs: Returns the common type or NULL on incompatibility.
// Invariants/Assumptions: Pointer types are not valid inputs here.
struct Type* get_common_type(struct Type* type1, struct Type* type2);

// Purpose: Compute a common pointer type for conditional/equality expressions.
// Inputs: expr1 and expr2 are the operand expressions.
// Outputs: Returns a common pointer type or NULL if incompatible.
// Invariants/Assumptions: Null pointer constants are allowed.
struct Type* get_common_pointer_type(struct Expr* expr1, struct Expr* expr2);

// Purpose: Convert an expression to a target type by inserting a cast.
// Inputs: expr is the expression pointer; target_type is the desired type.
// Outputs: Rewrites *expr when conversion is needed.
// Invariants/Assumptions: Target type pointer is reused as the value_type.
void convert_expr_type(struct Expr** expr, struct Type* target_type);

// Purpose: Determine if an expression is an lvalue.
// Inputs: expr is the expression node.
// Outputs: Returns true if the expression is assignable.
// Invariants/Assumptions: Only variables and dereferences are lvalues here.
bool is_lvalue(struct Expr* expr);

bool is_assignable(struct Expr* expr);

// Purpose: Compute the static initializer kind for a variable type.
// Inputs: type of the variable declaration.
// Outputs: Returns a StaticInitType enum value.
// Invariants/Assumptions: Only integer-like types are supported.
enum StaticInitType get_var_init(struct Type* var_dclr);

// Purpose: Compute the size of a type in bytes.
// Inputs: type is the Type node.
// Outputs: Returns the size in bytes or 0 for unknown types.
// Invariants/Assumptions: Uses target-specific sizes for integers/pointers.
size_t get_type_size(struct Type* type);

size_t get_type_alignment(struct Type* type);

// ------------------------- Symbol Table Functions ------------------------- //

// Purpose: Allocate a symbol table with a given bucket count.
// Inputs: numBuckets is the number of hash buckets.
// Outputs: Returns an allocated SymbolTable.
// Invariants/Assumptions: Caller must not free entries individually.
struct SymbolTable* create_symbol_table(size_t numBuckets);

// Purpose: Insert a symbol entry into the table.
// Inputs: hmap is the symbol table; key/type/attrs define the symbol.
// Outputs: Updates the table in place.
// Invariants/Assumptions: key/type/attrs pointers remain valid.
void symbol_table_insert(struct SymbolTable* hmap, struct Slice* key, struct Type* type, struct IdentAttr* attrs);

// Purpose: Look up a symbol by name.
// Inputs: hmap is the symbol table; key is the identifier name.
// Outputs: Returns the SymbolEntry or NULL if missing.
// Invariants/Assumptions: hash_slice is consistent with insertions.
struct SymbolEntry* symbol_table_get(struct SymbolTable* hmap, struct Slice* key);

// Purpose: Check whether a symbol exists in the table.
// Inputs: hmap is the symbol table; key is the identifier name.
// Outputs: Returns true if the symbol is present.
// Invariants/Assumptions: Does not distinguish between shadowing entries.
bool symbol_table_contains(struct SymbolTable* hmap, struct Slice* key);

void print_symbol_table(struct SymbolTable* hmap);

// ------------------------- Type Table Functions ------------------------- //

// Purpose: Allocate a type table with a given bucket count.
// Inputs: numBuckets is the number of hash buckets.
// Outputs: Returns an allocated TypeTable.
// Invariants/Assumptions: Caller must not free entries individually.
struct TypeTable* create_type_table(size_t numBuckets);

// Purpose: Insert a type entry into the table.
// Inputs: hmap is the type table; key/type/data define the entry.
// Outputs: Updates the table in place.
// Invariants/Assumptions: key pointers remain valid.
void type_table_insert(struct TypeTable* hmap, struct Slice* key,
    enum TypeEntryType type, union TypeEntryVariant data);

// Purpose: Look up a type entry by name.
// Inputs: hmap is the type table; key is the identifier name.
// Outputs: Returns the TypeEntry or NULL if missing.
// Invariants/Assumptions: hash_slice is consistent with insertions.
struct TypeEntry* type_table_get(struct TypeTable* hmap, struct Slice* key);

// Purpose: Check whether a type entry exists in the table.
// Inputs: hmap is the type table; key is the identifier name.
// Outputs: Returns true if the entry is present.
// Invariants/Assumptions: Does not distinguish between shadowing entries.
bool type_table_contains(struct TypeTable* hmap, struct Slice* key);

void print_type_table(struct TypeTable* hmap);

struct MemberEntry* get_struct_member(struct Type* type, struct Slice* member_name);

void print_ident_attr(struct IdentAttr* attrs);

void print_ident_init(struct IdentInit* init);

struct Initializer* make_zero_initializer(struct Type* type);

bool eval_const(struct Expr* expr, uint64_t* out_value);

struct InitList* is_init_const(struct Type* type, struct Initializer* init);

#endif // TYPECHECKING_H
