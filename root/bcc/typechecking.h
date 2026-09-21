#ifndef TYPECHECKING_H
#define TYPECHECKING_H

#include "AST.h"
#include "slice.h"

#include "../crt/stdbool.h"
#include "../crt/stdint.h"

// Declare typechecking metadata, symbol tables, and pass entry points.

struct IdentAttr;
struct InitList;

// Entry in the symbol table for a single identifier.
// Used by typechecker to validate uses and linkage.
// Key/type/attrs pointers remain valid for pass lifetime.
struct SymbolEntry{
  struct Slice* key;
  struct Type* type;
  struct IdentAttr* attrs;
  struct SymbolEntry* next;
};

// Distinguish struct, union, and named-type table entries.
enum TypeEntryType {
  STRUCT_ENTRY,
  UNION_ENTRY,
  ENUM_ENTRY,
};

// Store one aggregate member's name, type, offset, and successor.
struct MemberEntry{
  struct Slice* key;
  struct Type* type;
  size_t offset;
  struct MemberEntry* next;
};

// Store aggregate layout metadata and its member chain.
struct StructEntry{
  struct Slice* key;
  unsigned alignment;
  unsigned size;
  struct MemberEntry* members;
};

// Select struct or union layout payload for a type entry.
union TypeEntryVariant {
  struct StructEntry* struct_entry;
  struct StructEntry* union_entry;
};

// Store a named type entry and link it into the type table bucket.
struct TypeEntry{
  struct Slice* key;
  enum TypeEntryType type;
  union TypeEntryVariant data;
  struct TypeEntry* next;
};

// Hash table mapping type names to type entries.
// Used as a type table in typechecking.
struct TypeTable{
  size_t size;
  struct TypeEntry** arr;
};

// Hash table mapping identifier names to symbol entries.
// Used as the global symbol table in typechecking.
struct SymbolTable{
	size_t size;
  struct SymbolEntry** arr;
};

// Describe the category of identifier in the symbol table.
// Guides typechecking and linkage rules.
enum IdentAttrType {
  FUN_ATTR,
  STATIC_ATTR,
  LOCAL_ATTR,
  CONST_ATTR,
};

// Distinguish declarations, tentative definitions, and explicit static initializers.
enum IdentInitType {
  NO_INIT = 0,
  TENTATIVE = 1,
  INITIAL = 2
};

// Describe the initializer state for a static or global variable.
// Stored in IdentAttr for later code generation.
struct IdentInit {
  enum IdentInitType init_type;
  struct InitList* init_list; // valid if init_type == INITIAL
};

// Enumerate the kinds of static initialization supported.
// Guides data emission for static variables.
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

// Store the scalar, string, or pointer payload of a static initializer.
union StaticInitValue {
  uint64_t num;
  struct Slice* string; // for STRING_INIT
  struct Slice* pointer;     // for POINTER_INIT
};

// Store one typed static-initializer value in the declaration's layout.
// Used by InitList to describe static data.
struct StaticInit {
  enum StaticInitType int_type;
  union StaticInitValue value;
};

// Singly linked list of static initializer values.
// Stored in IdentInit for static variable initialization.
struct InitList {
  struct StaticInit* value;
  struct InitList* next;
};

// Attributes associated with a symbol table entry.
// Used by typechecking and later codegen phases.
struct IdentAttr {
  enum IdentAttrType attr_type;
  bool is_defined;
  enum StorageClass storage;
  struct IdentInit init;
  struct Slice* cleanup_handler;
};

// Global symbol table for the active typechecking pass.
// Used for symbol lookup across the translation unit.
extern struct SymbolTable* global_symbol_table;
extern struct TypeTable* global_type_table;
extern struct Type kCharType;

// ------------------------- Typechecking Functions ------------------------- //

// Typecheck every declaration in a program.
// Returns true on success; false on any type error.
bool typecheck_program(struct Program* program);

// Typecheck a file-scope declaration.
// Returns true on success; false on any type error.
bool typecheck_file_scope_dclr(struct Declaration* dclr);

// Typecheck a file-scope variable declaration/definition.
// Returns true on success; false on any type error.
// Initializers must be literal constants for globals.
bool typecheck_file_scope_var(struct VariableDclr* var_dclr);

// Typecheck a function declaration or definition.
// Returns true on success; false on any type error.
bool typecheck_func(struct FunctionDclr* func_dclr);

bool typecheck_struct(struct StructDclr* struct_dclr);

bool typecheck_union(struct UnionDclr* union_dclr);

bool typecheck_enum(struct EnumDclr* enum_dclr);

// Typecheck and convert an initializer to a target type.
// Returns true on success; false on any conversion/type errors.
bool typecheck_init(struct Initializer* init, struct Type* type);

// Typecheck an expression subtree and assign value_type.
// Returns true on success; false on any type error.
bool typecheck_expr(struct Expr* expr);

// Typecheck an expression with conversion rules.
// Returns true on success; false on any type error.
bool typecheck_convert_expr(struct Expr** expr);

// Typecheck parameter declarations for a function.
// Returns true on success; false on any type error.
bool typecheck_params(struct ParamList* params);

// Typecheck each item in a block.
// Returns true on success; false on any type error.
bool typecheck_block(struct Block* block);

// Typecheck a local declaration (variable or function).
// Returns true on success; false on any type error.
bool typecheck_local_dclr(struct Declaration* dclr);

// Typecheck a statement subtree.
// Returns true on success; false on any type error.
bool typecheck_stmt(struct Statement* stmt);

// Typecheck a local variable declaration/definition.
// Returns true on success; false on any type error.
bool typecheck_local_var(struct VariableDclr* var_dclr);

// Typecheck the initializer portion of a for loop.
// Returns true on success; false on any type error.
// For-init may be a declaration or expression.
bool typecheck_for_init(struct ForInit* init_);

// Typecheck a function call argument list.
// Returns true on success; false on any type error.
bool typecheck_args(struct ArgList* args, struct ParamTypeList* params, struct Expr* call_site);

// ------------------------- Type Utility Functions ------------------------- //

// Check whether a type is an arithmetic type.
// Returns true for integer-like types.
bool is_arithmetic_type(struct Type* type);

bool is_char_type(struct Type* type);

// Check whether a type is signed.
// Returns true for signed integer types.
bool is_signed_type(struct Type* type);

// Check whether a type is unsigned.
// Returns true for unsigned integer types.
bool is_unsigned_type(struct Type* type);

// Check whether a type is a pointer type.
// Returns true if type->type == POINTER_TYPE.
bool is_pointer_type(struct Type* type);

bool is_void_pointer_type(struct Type* type);

bool is_complete_type(struct Type* type);

bool is_pointer_to_complete_type(struct Type* type);

bool is_valid_type_specifier(struct Type* type);

// scalar types include arithmetic types and pointer types
// do not include arrays, functions, or void
bool is_scalar_type(struct Type* type);

// Check whether an expression is a null pointer constant.
// Returns true if expr is a null pointer constant.
bool is_null_pointer_constant(struct Expr* expr);

// Apply assignment conversions to an expression.
// Returns true on success; false on invalid conversions.
bool convert_by_assignment(struct Expr** expr, struct Type* target_type);

// Compute the common arithmetic type of two types.
// Returns the common type or NULL on incompatibility.
struct Type* get_common_type(struct Type* type1, struct Type* type2);

// Compute a common pointer type for conditional/equality expressions.
// Returns a common pointer type or NULL if incompatible.
struct Type* get_common_pointer_type(struct Expr* expr1, struct Expr* expr2);

// Convert an expression to a target type by inserting a cast.
// Rewrites *expr when conversion is needed.
void convert_expr_type(struct Expr** expr, struct Type* target_type);

// Determine if an expression is an lvalue.
// Returns true if the expression is assignable.
bool is_lvalue(struct Expr* expr);

bool is_assignable(struct Expr* expr);

// Compute the static initializer kind for a variable type.
// Returns a StaticInitType enum value.
enum StaticInitType get_var_init(struct Type* var_dclr);

// Compute the size of a type in bytes.
// Returns the size in bytes or 0 for unknown types.
size_t get_type_size(struct Type* type);

size_t get_type_alignment(struct Type* type);

// ------------------------- Symbol Table Functions ------------------------- //

// Allocate a symbol table with a given bucket count.
// Returns an allocated SymbolTable.
// Caller must not free entries individually.
struct SymbolTable* create_symbol_table(size_t numBuckets);

// Insert a symbol entry into the table.
// Key/type/attrs pointers remain valid.
void symbol_table_insert(struct SymbolTable* hmap, struct Slice* key, struct Type* type, struct IdentAttr* attrs);

// Look up a symbol by name.
// Returns the SymbolEntry or NULL if missing.
struct SymbolEntry* symbol_table_get(struct SymbolTable* hmap, struct Slice* key);

// Check whether a symbol exists in the table.
// Returns true if the symbol is present.
bool symbol_table_contains(struct SymbolTable* hmap, struct Slice* key);

void print_symbol_table(struct SymbolTable* hmap);

// ------------------------- Type Table Functions ------------------------- //

// Allocate a type table with a given bucket count.
// Returns an allocated TypeTable.
// Caller must not free entries individually.
struct TypeTable* create_type_table(size_t numBuckets);

// Insert a type entry into the table.
// Key pointers remain valid.
void type_table_insert(struct TypeTable* hmap, struct Slice* key,
    enum TypeEntryType type, union TypeEntryVariant data);

// Look up a type entry by name.
// Returns the TypeEntry or NULL if missing.
struct TypeEntry* type_table_get(struct TypeTable* hmap, struct Slice* key);

// Check whether a type entry exists in the table.
// Returns true if the entry is present.
bool type_table_contains(struct TypeTable* hmap, struct Slice* key);

void print_type_table(struct TypeTable* hmap);

struct MemberEntry* get_struct_member(struct Type* type, struct Slice* member_name);

void print_ident_attr(struct IdentAttr* attrs);

void print_ident_init(struct IdentInit* init);

struct Initializer* make_zero_initializer(struct Type* type);

bool eval_const(struct Expr* expr, uint64_t* out_value);

struct InitList* is_init_const(struct Type* type, struct Initializer* init);

#endif // TYPECHECKING_H
