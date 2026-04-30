#include "typechecking.h"
#include "arena.h"
#include "source_location.h"
#include "unique_name.h"

#include "../crt/inttypes.h"
#include "../crt/stdio.h"
#include "../crt/print.h"
#include "../crt/stdlib.h"

// Purpose: Implement typechecking and symbol table utilities.
// Inputs: Operates on AST nodes produced by parsing and resolution.
// Outputs: Annotates expressions with types and validates declarations.
// Invariants/Assumptions: Typechecker uses a single global symbol table.

// Purpose: Global symbol table for the current typechecking pass.
// Inputs: Initialized in typecheck_program and used by all helpers.
// Outputs: Stores symbol entries for declarations and lookups.
// Invariants/Assumptions: Only one typechecking pass runs at a time.
struct SymbolTable* global_symbol_table = NULL;
struct TypeTable* global_type_table = NULL;

struct Type kIntType = { INT_TYPE };
struct Type kUIntType = { UINT_TYPE };
struct Type kCharType = { CHAR_TYPE };
struct Type kVoidType = { VOID_TYPE };

// Purpose: Identify compound assignment operators.
// Inputs: op is the binary operator enum value.
// Outputs: Returns true for +=, -=, etc.
// Invariants/Assumptions: ASSIGN_OP is handled separately.
static bool is_compound_assign_op(enum BinOp op) {
  switch (op) {
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
      return true;
    default:
      return false;
  }
}

// Purpose: Map a compound assignment operator to its base binary operator.
// Inputs: op must satisfy is_compound_assign_op(op).
// Outputs: Returns the corresponding arithmetic/bitwise operator.
// Invariants/Assumptions: Caller validates op.
static enum BinOp compound_assign_base_op(enum BinOp op) {
  switch (op) {
    case PLUS_EQ_OP:
      return ADD_OP;
    case MINUS_EQ_OP:
      return SUB_OP;
    case MUL_EQ_OP:
      return MUL_OP;
    case DIV_EQ_OP:
      return DIV_OP;
    case MOD_EQ_OP:
      return MOD_OP;
    case AND_EQ_OP:
      return BIT_AND;
    case OR_EQ_OP:
      return BIT_OR;
    case XOR_EQ_OP:
      return BIT_XOR;
    case SHL_EQ_OP:
      return BIT_SHL;
    case SHR_EQ_OP:
      return BIT_SHR;
    default:
      return op;
  }
}

// Purpose: Emit a formatted type error at a source location.
// Inputs: loc points into source text; message is a fixed diagnostic string.
// Outputs: Writes a diagnostic message to stdout.
// Invariants/Assumptions: source_location_from_ptr handles NULL/unknown locations.
static void type_error_at(char* loc, char* message) {
  struct SourceLocation where = source_location_from_ptr(loc);
  char* filename = source_filename_for_ptr(loc);

  if (where.line == 0) {
    fdputs(STDOUT, "Type error: ");
    fdputs(STDOUT, message);
  } else {
    int args[3];

    args[0] = (int)filename;
    args[1] = (int)where.line;
    args[2] = (int)where.column;
    fdprintf(STDOUT, "Type error at %s:%zu:%zu: ", args);
  }
  fdputs(STDOUT, "\n");
}

static void type_error_at1_slice(char* loc, char* fmt, size_t arg0_len, char* arg0_text) {
  int args[2];
  struct SourceLocation where = source_location_from_ptr(loc);
  char* filename = source_filename_for_ptr(loc);

  args[0] = (int)arg0_len;
  args[1] = (int)arg0_text;
  if (where.line == 0) {
    fdputs(STDOUT, "Type error: ");
  } else {
    int prefix_args[3];

    prefix_args[0] = (int)filename;
    prefix_args[1] = (int)where.line;
    prefix_args[2] = (int)where.column;
    fdprintf(STDOUT, "Type error at %s:%zu:%zu: ", prefix_args);
  }
  fdprintf(STDOUT, fmt, args);
  fdputs(STDOUT, "\n");
}

static void type_error_at2_slice(char* loc, char* fmt,
                                 size_t arg0_len, char* arg0_text,
                                 size_t arg1_len, char* arg1_text) {
  int args[4];
  struct SourceLocation where = source_location_from_ptr(loc);
  char* filename = source_filename_for_ptr(loc);

  args[0] = (int)arg0_len;
  args[1] = (int)arg0_text;
  args[2] = (int)arg1_len;
  args[3] = (int)arg1_text;
  if (where.line == 0) {
    fdputs(STDOUT, "Type error: ");
  } else {
    int prefix_args[3];

    prefix_args[0] = (int)filename;
    prefix_args[1] = (int)where.line;
    prefix_args[2] = (int)where.column;
    fdprintf(STDOUT, "Type error at %s:%zu:%zu: ", prefix_args);
  }
  fdprintf(STDOUT, fmt, args);
  fdputs(STDOUT, "\n");
}

static void type_error_at2_size(char* loc, char* fmt, size_t arg0, size_t arg1) {
  int args[2];
  struct SourceLocation where = source_location_from_ptr(loc);
  char* filename = source_filename_for_ptr(loc);

  args[0] = (int)arg0;
  args[1] = (int)arg1;
  if (where.line == 0) {
    fdputs(STDOUT, "Type error: ");
  } else {
    int prefix_args[3];

    prefix_args[0] = (int)filename;
    prefix_args[1] = (int)where.line;
    prefix_args[2] = (int)where.column;
    fdprintf(STDOUT, "Type error at %s:%zu:%zu: ", prefix_args);
  }
  fdprintf(STDOUT, fmt, args);
  fdputs(STDOUT, "\n");
}

// Purpose: Merge storage classes across compatible declarations.
// Inputs: existing is the prior storage; incoming is the new declaration storage.
// Outputs: Returns the combined storage, preserving internal linkage if present.
// Invariants/Assumptions: Caller has already validated linkage compatibility.
static enum StorageClass merge_storage_class(enum StorageClass existing,
                                             enum StorageClass incoming) {
  if (existing == STATIC || incoming == STATIC) {
    return STATIC;
  }
  if (existing == EXTERN || incoming == EXTERN) {
    return EXTERN;
  }
  return NONE;
}

// ------------------------- Typechecking Functions ------------------------- //

// Purpose: Typecheck every declaration in a program.
// Inputs: program is the Program AST.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Initializes global_symbol_table for this pass.
bool typecheck_program(struct Program* program) {
  global_symbol_table = create_symbol_table(1024);
  global_type_table = create_type_table(1024);

  // typecheck each declaration in the program
  for (struct DeclarationList* cur = program->dclrs; cur != NULL; cur = cur->next){
    if (!typecheck_file_scope_dclr(&cur->dclr)) {
      return false;
    }
  }
  return true;
}

// Purpose: Typecheck a file-scope declaration.
// Inputs: dclr is the declaration node.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: File-scope symbols are stored in global_symbol_table.
bool typecheck_file_scope_dclr(struct Declaration* dclr) {
  switch (dclr->type) {
    case VAR_DCLR:
      return typecheck_file_scope_var(&dclr->dclr.var_dclr);
    case FUN_DCLR:
      return typecheck_func(&dclr->dclr.fun_dclr);
    case STRUCT_DCLR:
      return typecheck_struct(&dclr->dclr.struct_dclr);
    case UNION_DCLR:
      return typecheck_union(&dclr->dclr.union_dclr);
    case ENUM_DCLR:
      return typecheck_enum(&dclr->dclr.enum_dclr);
    default:
      type_error_at(NULL, "unknown declaration type in typecheck_file_scope_dclr");
      return false; // Unknown declaration type
  }
}

// Purpose: Typecheck a file-scope variable declaration/definition.
// Inputs: var_dclr is the variable declaration node.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Global initializers must be constant literals.
bool typecheck_file_scope_var(struct VariableDclr* var_dclr) {
  enum IdentInitType init_type = -1;
  // Infer file-scope initialization status from storage class and initializer.
  if (var_dclr->init != NULL) {
    init_type = INITIAL;
  } else if (var_dclr->storage != EXTERN) {
    init_type = TENTATIVE;
  } else {
    init_type = NO_INIT;
  }

  if (!is_complete_type(var_dclr->type)) {
    type_error_at1_slice(var_dclr->name->start, "incomplete type for variable %.*s", (int)var_dclr->name->len, var_dclr->name->start);
    return false;
  }
  if (!is_valid_type_specifier(var_dclr->type)) {
    type_error_at1_slice(var_dclr->name->start, "invalid type for variable %.*s", (int)var_dclr->name->len, var_dclr->name->start);
    return false;
  }

  // if there is an initializer, typecheck it
  struct InitList* init_list = NULL;
  if (var_dclr->init != NULL) {
    if ((init_list = is_init_const(var_dclr->type, var_dclr->init)) == NULL) {
      // global variable initializers must be constant
      type_error_at1_slice(var_dclr->init->loc, "non-constant initializer for global variable %.*s", (int)var_dclr->name->len, var_dclr->name->start);
      return false;
    }

    if (!typecheck_init(var_dclr->init, var_dclr->type)) {
      return false;
    }
  }

  struct SymbolEntry* entry = symbol_table_get(global_symbol_table, var_dclr->name);

  // check if this variable has been declared before
  if (entry != NULL) {

    // reject function types
    if (entry->type->type == FUN_TYPE) {
      type_error_at1_slice(var_dclr->name->start, "function %.*s redeclared as variable", (int)var_dclr->name->len, var_dclr->name->start);
      return false;
    }

    // ensure both declarations have the same type
    if (!compare_types(entry->type, var_dclr->type)) {
      type_error_at1_slice(var_dclr->name->start, "conflicting declarations for variable %.*s", (int)var_dclr->name->len, var_dclr->name->start);
      return false;
    }

    // check for duplicate definitions
    if (entry->attrs->init.init_type == INITIAL && var_dclr->init != NULL) {
      type_error_at1_slice(var_dclr->name->start, "conflicting file scope variable definitions for variable %.*s", (int)var_dclr->name->len, var_dclr->name->start);
      return false;
    }

    // check for conflicting linkage (internal vs external)
    bool entry_internal = (entry->attrs->storage == STATIC);
    bool dclr_internal = (var_dclr->storage == EXTERN) ? entry_internal
                                                       : (var_dclr->storage == STATIC);
    if (entry_internal != dclr_internal) {
      type_error_at1_slice(var_dclr->name->start, "conflicting variable linkage for variable %.*s", (int)var_dclr->name->len, var_dclr->name->start);
      return false;
    }

    // By this point, types and linkage match; merge storage for explicit externs.
    entry->attrs->storage = merge_storage_class(entry->attrs->storage, var_dclr->storage);

    // Update init state if it improves. Ordering is NO_INIT < TENTATIVE < INITIAL.

    if (init_type > entry->attrs->init.init_type) {
      // upgrade init type
      entry->attrs->init.init_type = init_type;
      entry->attrs->is_defined = (init_type == INITIAL);
      entry->attrs->init.init_list = init_list;
    }
  } else {
    // new declaration, add to symbol table
    struct IdentAttr* attrs = arena_alloc(sizeof(struct IdentAttr));
    attrs->attr_type = STATIC_ATTR;
    attrs->is_defined = (init_type == INITIAL);
    attrs->storage = var_dclr->storage;
    attrs->init.init_type = init_type;
    attrs->init.init_list = init_list;
    attrs->cleanup_handler = NULL;

    symbol_table_insert(global_symbol_table, var_dclr->name, var_dclr->type, attrs);
  }

  return true;
}

// works for unions as well
bool validate_struct_definition(struct Slice* struct_name, struct MemberDclr* first_member){
  // ensure struct has not been defined before
  struct TypeEntry* entry = type_table_get(global_type_table, struct_name);
  if (entry != NULL){
    type_error_at1_slice(struct_name->start, "redefinition of struct/union %.*s", (int)struct_name->len, struct_name->start);
    return false;
  }

  // ensure no duplicate member names
  struct MemberDclr* member_cur = first_member;
  while (member_cur != NULL){
    struct MemberDclr* member_check = member_cur->next;

    if (!is_valid_type_specifier(member_cur->type)){
      type_error_at2_slice(member_cur->name->start, "incomplete type for member %.*s in struct %.*s", (int)member_cur->name->len, member_cur->name->start, (int)struct_name->len, struct_name->start);
      return false;
    }

    while (member_check != NULL){
      if (compare_slice_to_slice(member_cur->name, member_check->name)){
        type_error_at2_slice(member_check->name->start, "duplicate member name %.*s in struct %.*s", (int)member_check->name->len, member_check->name->start, (int)struct_name->len, struct_name->start);
        return false;
      }
      member_check = member_check->next;
    }
    member_cur = member_cur->next;
  }

  return true;
}

bool typecheck_struct(struct StructDclr* struct_dclr){
  if (struct_dclr->members == NULL){
    // forward declaration, nothing to typecheck
    return true;
  }

  if (!validate_struct_definition(struct_dclr->name, struct_dclr->members)){
    return false;
  }

  struct MemberEntry* member_entries = NULL;
  struct MemberEntry* member_entries_tail = NULL;
  size_t offset = 0;
  size_t alignment = 1;

  for (struct MemberDclr* member_cur = struct_dclr->members; member_cur != NULL; member_cur = member_cur->next){
    size_t member_alignment = get_type_alignment(member_cur->type);
    if (member_alignment == -1){
      type_error_at2_slice(member_cur->name->start, "incomplete type for member %.*s in struct %.*s", (int)member_cur->name->len, member_cur->name->start, (int)struct_dclr->name->len, struct_dclr->name->start);
      return false;
    }

    // align current offset to member size
    if (offset % member_alignment != 0){
      offset += member_alignment - (offset % member_alignment);
    }

    struct MemberEntry* member_entry = arena_alloc(sizeof(struct MemberEntry));
    member_entry->key = member_cur->name;
    member_entry->type = member_cur->type;
    member_entry->offset = offset;
    member_entry->next = NULL;
    if (member_entries == NULL){
      member_entries = member_entry;
      member_entries_tail = member_entry;
    } else {
      member_entries_tail->next = member_entry;
      member_entries_tail = member_entry;
    }

    offset += get_type_size(member_cur->type);

    // alignment of struct is max of member alignments
    if (member_alignment > alignment){
      alignment = member_alignment;
    }
  }

  // round offset to struct alignment
  if (offset % alignment != 0){
    offset += alignment - (offset % alignment);
  }

  struct StructEntry* struct_entry = arena_alloc(sizeof(struct StructEntry));
  struct_entry->key = struct_dclr->name;
  struct_entry->alignment = alignment;
  struct_entry->size = offset;
  struct_entry->members = member_entries;

  union TypeEntryVariant entry_data;
  entry_data.struct_entry = struct_entry;

  type_table_insert(global_type_table, struct_dclr->name, STRUCT_ENTRY, entry_data);

  return true;
}

bool typecheck_union(struct UnionDclr* union_dclr){
  if (union_dclr->members == NULL){
    // forward declaration, nothing to typecheck
    return true;
  }

  if (!validate_struct_definition(union_dclr->name, union_dclr->members)){
    return false;
  }

  struct MemberEntry* member_entries = NULL;
  struct MemberEntry* member_entries_tail = NULL;
  size_t size = 0;
  size_t alignment = 1;

  for (struct MemberDclr* member_cur = union_dclr->members; member_cur != NULL; member_cur = member_cur->next){
    size_t member_alignment = get_type_alignment(member_cur->type);
    if (member_alignment == -1){
      type_error_at2_slice(member_cur->name->start, "incomplete type for member %.*s in struct %.*s", (int)member_cur->name->len, member_cur->name->start, (int)union_dclr->name->len, union_dclr->name->start);
      return false;
    }

    struct MemberEntry* member_entry = arena_alloc(sizeof(struct MemberEntry));
    member_entry->key = member_cur->name;
    member_entry->type = member_cur->type;
    member_entry->offset = 0; // all members at offset 0 in union
    member_entry->next = NULL;
    if (member_entries == NULL){
      member_entries = member_entry;
      member_entries_tail = member_entry;
    } else {
      member_entries_tail->next = member_entry;
      member_entries_tail = member_entry;
    }

    // union size is max of member sizes
    size_t member_size = get_type_size(member_cur->type);
    if (member_size > size){
      size = member_size;
    }

    // alignment of union is max of member alignments
    if (member_alignment > alignment){
      alignment = member_alignment;
    }
  }

  // round size to union alignment
  if (size % alignment != 0){
    size += alignment - (size % alignment);
  }

  struct StructEntry* union_entry = arena_alloc(sizeof(struct StructEntry));
  union_entry->key = union_dclr->name;
  union_entry->alignment = alignment;
  union_entry->size = size;
  union_entry->members = member_entries;
  union TypeEntryVariant entry_data;
  entry_data.union_entry = union_entry;

  type_table_insert(global_type_table, union_dclr->name, UNION_ENTRY, entry_data);

  return true;
}

bool typecheck_enum(struct EnumDclr* enum_dclr){
  if (enum_dclr->members == NULL){
    // no forward declarations for enums
    type_error_at(NULL, "enum forward declarations are not supported");
    return false;
  }

  // ensure enum has not been defined before
  struct TypeEntry* entry = type_table_get(global_type_table, enum_dclr->name);
  if (entry != NULL){
    type_error_at1_slice(enum_dclr->name->start, "redefinition of enum %.*s", (int)enum_dclr->name->len, enum_dclr->name->start);
    return false;
  }

  struct TypeEntry* enum_entry = arena_alloc(sizeof(struct TypeEntry));
  enum_entry->key = enum_dclr->name;
  enum_entry->type = ENUM_ENTRY;
  enum_entry->next = NULL;

  union TypeEntryVariant entry_data;
  type_table_insert(global_type_table, enum_dclr->name, ENUM_ENTRY, entry_data);

  return true;
}

// Purpose: Apply array-to-pointer decay to function parameter types.
// Inputs: func_dclr is the function declaration to update.
// Outputs: Updates both parameter lists in-place.
// Invariants/Assumptions: Param list and type list are in sync.
static void decay_param_array_types(struct FunctionDclr* func_dclr) {
  if (func_dclr == NULL || func_dclr->type == NULL ||
      func_dclr->type->type != FUN_TYPE) {
    return;
  }
  struct ParamList* param_cur = func_dclr->params;
  struct ParamTypeList* type_cur = func_dclr->type->type_data.fun_type.param_types;
  for (; param_cur != NULL && type_cur != NULL;
       param_cur = param_cur->next, type_cur = type_cur->next) {
    if (param_cur->param.type != NULL &&
        param_cur->param.type->type == ARRAY_TYPE) {
      struct Type* array_type = param_cur->param.type;
      struct Type* pointer_type = arena_alloc(sizeof(struct Type));
      pointer_type->type = POINTER_TYPE;
      pointer_type->type_data.pointer_type.referenced_type =
          array_type->type_data.array_type.element_type;
      param_cur->param.type = pointer_type;
      type_cur->type = pointer_type;
      // Keep the parameter symbol type in sync with the decayed pointer type.
      if (param_cur->param.name != NULL) {
        struct SymbolEntry* entry =
            symbol_table_get(global_symbol_table, param_cur->param.name);
        if (entry != NULL) {
          entry->type = pointer_type;
        }
      }
    }
  }
}

// Purpose: Typecheck a function declaration or definition.
// Inputs: func_dclr is the function declaration node.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Parameters and body share the global symbol table.
bool typecheck_func(struct FunctionDclr* func_dclr) {
  // Parameters share the same symbol table as the body in this pass.
  // typecheck them before decaying array types, so that incomplete array
  // types are caught as errors.
  if (!typecheck_params(func_dclr->params)) {
    return false;
  }

  decay_param_array_types(func_dclr);
  struct SymbolEntry* entry = symbol_table_get(global_symbol_table, func_dclr->name);

  if (entry == NULL) {
    // ensure return type is not array
    if (func_dclr->type->type == ARRAY_TYPE) {
      type_error_at1_slice(func_dclr->name->start, "function %.*s cannot have array return type", (int)func_dclr->name->len, func_dclr->name->start);
      return false;
    }

    // First declaration/definition of this function.
    struct IdentAttr* attrs = arena_alloc(sizeof(struct IdentAttr));
    attrs->attr_type = FUN_ATTR;
    attrs->is_defined = (func_dclr->body != NULL);
    attrs->storage = func_dclr->storage;
    attrs->cleanup_handler = NULL;
    symbol_table_insert(global_symbol_table, func_dclr->name, func_dclr->type, attrs);
  } else {
    // ensure the existing entry is a function
    if (entry->type->type != FUN_TYPE) {
      type_error_at1_slice(func_dclr->name->start, "variable %.*s redeclared as function", (int)func_dclr->name->len, func_dclr->name->start);
      return false;
    }

    // ensure both declarations have the same type
    if (!compare_types(entry->type, func_dclr->type)) {
      type_error_at1_slice(func_dclr->name->start, "conflicting declarations for function %.*s", (int)func_dclr->name->len, func_dclr->name->start);
      return false;
    }

    // check for duplicate definitions
    if (entry->attrs->is_defined && func_dclr->body != NULL) {
      type_error_at1_slice(func_dclr->name->start, "multiple definitions for function %.*s", (int)func_dclr->name->len, func_dclr->name->start);
      return false;
    }

    // check for conflicting linkage
    // extern matches previous linkage, cannot cause conflict
    enum StorageClass effective_storage = func_dclr->storage;
    if (effective_storage == NONE && entry->attrs->storage != NONE) {
      // Block/file-scope declarations without storage inherit visible linkage.
      effective_storage = entry->attrs->storage;
    }
    if (entry->attrs->storage != effective_storage &&
       (effective_storage != EXTERN) &&
       (entry->attrs->storage != EXTERN)) {
      type_error_at1_slice(func_dclr->name->start, "conflicting function linkage for function %.*s", (int)func_dclr->name->len, func_dclr->name->start);
      return false;
    }

    // update definition status
    if (func_dclr->body != NULL) {
      entry->attrs->is_defined = true;
    }

    entry->attrs->storage = merge_storage_class(entry->attrs->storage, effective_storage);
  }

  if (func_dclr->body != NULL) {
    // typecheck function body
    if (!typecheck_block(func_dclr->body)) {
      return false;
    }
  }

  return true;
}

// Purpose: Typecheck and register each function parameter.
// Inputs: params is the parameter list.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Parameters are inserted into global_symbol_table.
bool typecheck_params(struct ParamList* params) {
  struct ParamList* cur = params;
  while (cur != NULL) {
    // ensure each parameter has no initializer
    if (cur->param.init != NULL) {
      type_error_at1_slice(cur->param.name->start, "function parameter %.*s should not have an initializer", (int)cur->param.name->len, cur->param.name->start);
      return false;
    }

    if (!is_valid_type_specifier(cur->param.type)) {
      type_error_at1_slice(cur->param.name->start, "invalid type specifier for function parameter %.*s", (int)cur->param.name->len, cur->param.name->start);
      return false;
    }

    // Add parameters as locals so body expressions can reference them.
    struct IdentAttr* attrs = arena_alloc(sizeof(struct IdentAttr));
    attrs->attr_type = LOCAL_ATTR;
    attrs->is_defined = true;
    attrs->storage = NONE; // parameters have no storage class
    attrs->cleanup_handler = NULL;
    symbol_table_insert(global_symbol_table, cur->param.name, cur->param.type, attrs);

    cur = cur->next;
  }
  return true;
}

// Purpose: Typecheck each item in a block.
// Inputs: block is the block list.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Symbol table is shared across the function body.
bool typecheck_block(struct Block* block) {
  struct Block* cur = block;
  while (cur != NULL) {
    switch (cur->item->type) {
      case DCLR_ITEM:
        // typecheck local declaration
        if (!typecheck_local_dclr(cur->item->item.dclr)) {
          return false;
        }
        break;
      case STMT_ITEM:
        // typecheck statement
        if (!typecheck_stmt(cur->item->item.stmt)) {
          return false;
        }
        break;
      default:
        type_error_at(NULL, "unknown block item type in typecheck_block");
        return false; // Unknown block item type
    }
    cur = cur->next;
  }
  return true;
}

// Purpose: Typecheck a statement subtree.
// Inputs: stmt is the statement node.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Return statements reference the current function symbol.
bool typecheck_stmt(struct Statement* stmt) {
  switch (stmt->type) {
    // Placeholder implementation
    case RETURN_STMT: {
      if (stmt->statement.ret_stmt.expr != NULL && !typecheck_convert_expr(&stmt->statement.ret_stmt.expr)) {
        return false;
      }

      // Ensure the return value can be converted to the function's return type.
      struct SymbolEntry* entry = symbol_table_get(global_symbol_table, stmt->statement.ret_stmt.func);
      if (entry == NULL) {
        type_error_at(stmt->loc, "unknown function in return statement");
        return false;
      }
      struct Type* func_type = entry->type;
      struct Type* ret_type = func_type->type_data.fun_type.return_type;

      if (stmt->statement.ret_stmt.expr != NULL && !convert_by_assignment(&stmt->statement.ret_stmt.expr, ret_type)) {
        type_error_at(stmt->loc, "incompatible return type in return statement");
        return false;
      }

      if (ret_type->type == VOID_TYPE && stmt->statement.ret_stmt.expr != NULL) {
        type_error_at(stmt->loc, "void function cannot return a value");
        return false;
      }

      if (ret_type->type != VOID_TYPE && stmt->statement.ret_stmt.expr == NULL) {
        type_error_at(stmt->loc, "non-void function must return a value");
        return false;
      }

      break;
    }
    case EXPR_STMT: {
      if (!typecheck_convert_expr(&stmt->statement.expr_stmt.expr)) {
        return false;
      }
      break;
    }
    case IF_STMT: {
      if (!typecheck_convert_expr(&stmt->statement.if_stmt.condition)) {
        return false;
      }

      if (!is_scalar_type(stmt->statement.if_stmt.condition->value_type)) {
        type_error_at(stmt->statement.if_stmt.condition->loc,
                      "if condition must have scalar type");
        return false;
      }

      if (!is_arithmetic_type(stmt->statement.if_stmt.condition->value_type) &&
          !is_pointer_type(stmt->statement.if_stmt.condition->value_type)) {
        type_error_at(stmt->statement.if_stmt.condition->loc,
                      "if condition must have scalar type");
        return false;
      }

      if (!typecheck_stmt(stmt->statement.if_stmt.if_stmt)) {
        return false;
      }
      if (stmt->statement.if_stmt.else_stmt != NULL) {
        if (!typecheck_stmt(stmt->statement.if_stmt.else_stmt)) {
          return false;
        }
      }
      break;
    }
    case GOTO_STMT: {
      // nothing to typecheck
      break;
    }
    case LABELED_STMT: {
      if (!typecheck_stmt(stmt->statement.labeled_stmt.stmt)) {
        return false;
      }
      break;
    }
    case WHILE_STMT: {
      if (!typecheck_convert_expr(&stmt->statement.while_stmt.condition)) {
        return false;
      }

      if (!is_scalar_type(stmt->statement.while_stmt.condition->value_type)) {
        type_error_at(stmt->statement.while_stmt.condition->loc,
                      "while condition must have scalar type");
        return false;
      }

      if (!is_arithmetic_type(stmt->statement.while_stmt.condition->value_type) &&
          !is_pointer_type(stmt->statement.while_stmt.condition->value_type)) {
        type_error_at(stmt->statement.while_stmt.condition->loc,
                      "while condition must have scalar type");
        return false;
      }

      if (!typecheck_stmt(stmt->statement.while_stmt.statement)) {
        return false;
      }
      break;
    }
    case DO_WHILE_STMT: {
      if (!typecheck_stmt(stmt->statement.do_while_stmt.statement)) {
        return false;
      }
      if (!typecheck_convert_expr(&stmt->statement.do_while_stmt.condition)) {
        return false;
      }

      if (!is_scalar_type(stmt->statement.do_while_stmt.condition->value_type)) {
        type_error_at(stmt->statement.do_while_stmt.condition->loc,
                      "do-while condition must have scalar type");
        return false;
      }

      if (!is_arithmetic_type(stmt->statement.do_while_stmt.condition->value_type) &&
          !is_pointer_type(stmt->statement.do_while_stmt.condition->value_type)) {
        type_error_at(stmt->statement.do_while_stmt.condition->loc,
                      "do-while condition must have scalar type");
        return false;
      }

      break;
    }
    case FOR_STMT: {
      // Each part is optional, but any present expressions must be typed.
      if (!typecheck_for_init(stmt->statement.for_stmt.init)) {
        return false;
      }
      if (stmt->statement.for_stmt.condition != NULL) {
        if (!typecheck_convert_expr(&stmt->statement.for_stmt.condition)) {
          return false;
        }
      }
      if (stmt->statement.for_stmt.end != NULL) {
        if (!typecheck_convert_expr(&stmt->statement.for_stmt.end)) {
          return false;
        }
      }
      if (!typecheck_stmt(stmt->statement.for_stmt.statement)) {
        return false;
      }
      break;
    }
    case SWITCH_STMT: {
      if (!typecheck_convert_expr(&stmt->statement.switch_stmt.condition)) {
        return false;
      }

      if (!is_arithmetic_type(stmt->statement.switch_stmt.condition->value_type)) {
        type_error_at(stmt->statement.switch_stmt.condition->loc,
                      "switch condition must have arithmetic type");
        return false;
      }

      if (!typecheck_stmt(stmt->statement.switch_stmt.statement)) {
        return false;
      }
      break;
    }
    case CASE_STMT: {
      if (!typecheck_convert_expr(&stmt->statement.case_stmt.expr)) {
        return false;
      }
      if (!typecheck_stmt(stmt->statement.case_stmt.statement)) {
        return false;
      }
      break;
    }
    case DEFAULT_STMT: {
      if (!typecheck_stmt(stmt->statement.default_stmt.statement)) {
        return false;
      }
      break;
    }
    case BREAK_STMT: {
      // nothing to typecheck
      break;
    }
    case CONTINUE_STMT: {
      // nothing to typecheck
      break;
    }
    case COMPOUND_STMT: {
      if (!typecheck_block(stmt->statement.compound_stmt.block)) {
        return false;
      }
      break;
    }
    case NULL_STMT: {
      // nothing to typecheck
      break;
    }

    default: {
      type_error_at(stmt->loc, "unknown statement type in typecheck_stmt");
      return false; // Unknown statement type
    }
  }

  return true;
}

// Purpose: Typecheck the initializer portion of a for statement.
// Inputs: init_ is the ForInit node.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: For-init may be a declaration or expression.
bool typecheck_for_init(struct ForInit* init_) {
  switch (init_->type) {
    case DCLR_INIT:
      if (init_->init.dclr_init->storage != NONE) {
        type_error_at1_slice(init_->init.dclr_init->name->start, "storage class not allowed in for-loop initializer for variable %.*s", (int)init_->init.dclr_init->name->len, init_->init.dclr_init->name->start);
        return false;
      }
      return typecheck_local_var(init_->init.dclr_init);
    case EXPR_INIT:
      if (init_->init.expr_init != NULL) {
        return typecheck_convert_expr(&init_->init.expr_init);
      } else {
        return true; // Nothing to typecheck
      }
    default:
      type_error_at(NULL, "unknown for init type in typecheck_for_init");
      return false; // Unknown for init type
  }
}

// Purpose: Typecheck a local declaration (variable or function).
// Inputs: dclr is the declaration node.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Local declarations use the global symbol table.
bool typecheck_local_dclr(struct Declaration* dclr) {
  switch (dclr->type) {
    case VAR_DCLR:
      return typecheck_local_var(&dclr->dclr.var_dclr);
    case FUN_DCLR:
      return typecheck_func(&dclr->dclr.fun_dclr);
    case STRUCT_DCLR:
      return typecheck_struct(&dclr->dclr.struct_dclr);
    case UNION_DCLR:
      return typecheck_union(&dclr->dclr.union_dclr);
    case ENUM_DCLR:
      return typecheck_enum(&dclr->dclr.enum_dclr);
    default:
      type_error_at(NULL, "unknown declaration type in typecheck_local_dclr");
      return false; // Unknown declaration type
  }
}

// Purpose: Typecheck a local variable declaration/definition.
// Inputs: var_dclr is the variable declaration node.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Enforces extern/static/local linkage rules.
bool typecheck_local_var(struct VariableDclr* var_dclr) {
  if (!is_complete_type(var_dclr->type)) {
    type_error_at1_slice(var_dclr->name->start, "incomplete type for variable %.*s", (int)var_dclr->name->len, var_dclr->name->start);
    return false;
  }
  if (!is_valid_type_specifier(var_dclr->type)) {
    type_error_at1_slice(var_dclr->name->start, "invalid type for variable %.*s", (int)var_dclr->name->len, var_dclr->name->start);
    return false;
  }

  if (var_dclr->storage == EXTERN) {
    // Local extern declarations just validate and/or introduce a global symbol.
    if (var_dclr->init != NULL) {
      type_error_at1_slice(var_dclr->init->loc, "initializer on local extern variable declaration for variable %.*s", (int)var_dclr->name->len, var_dclr->name->start);
      return false;
    }

    struct SymbolEntry* entry = symbol_table_get(global_symbol_table, var_dclr->name);
    if (entry == NULL) {
      // New extern declaration shares the global table but has no definition.
      struct IdentAttr* attrs = arena_alloc(sizeof(struct IdentAttr));
      attrs->attr_type = STATIC_ATTR;
      attrs->is_defined = false;
      attrs->storage = EXTERN;
      attrs->init.init_type = NO_INIT;
      attrs->init.init_list = NULL;
      attrs->cleanup_handler = NULL;
      symbol_table_insert(global_symbol_table, var_dclr->name, var_dclr->type, attrs);
    } else {
      // ensure the existing entry is not a function
      if (entry->type->type == FUN_TYPE) {
        type_error_at1_slice(var_dclr->name->start, "function %.*s redeclared as variable", (int)var_dclr->name->len, var_dclr->name->start);
        return false;
      }

      // ensure both declarations have the same type
      if (!compare_types(entry->type, var_dclr->type)) {
        type_error_at1_slice(var_dclr->name->start, "conflicting declarations for variable %.*s", (int)var_dclr->name->len, var_dclr->name->start);
        return false;
      }

      entry->attrs->storage = merge_storage_class(entry->attrs->storage, EXTERN);
    }

    return true;
  } else if (var_dclr->storage == STATIC) {
    struct InitList* init_list = NULL;
    // Local static behaves like a file-scope object with local visibility.
    if (var_dclr->init != NULL) {
      if ((init_list = is_init_const(var_dclr->type, var_dclr->init)) == NULL) {
        // For simplicity, we only allow literal initializers for global variables
        type_error_at1_slice(var_dclr->init->loc, "non-constant initializer for global variable %.*s", (int)var_dclr->name->len, var_dclr->name->start);
        return false;
      }
      if (var_dclr->type->type == POINTER_TYPE) {
        struct Expr* init_expr = var_dclr->init->init.single_init;
        if (!is_null_pointer_constant(init_expr) && init_expr->type != STRING) {
          type_error_at1_slice(var_dclr->init->loc, "invalid pointer initializer for static local variable %.*s", (int)var_dclr->name->len, var_dclr->name->start);
          return false;
        }
      }
      if (!typecheck_init(var_dclr->init, var_dclr->type)) {
        return false;
      }
    }

    struct SymbolEntry* entry = symbol_table_get(global_symbol_table, var_dclr->name);
    if (entry == NULL) {
      struct IdentAttr* attrs = arena_alloc(sizeof(struct IdentAttr));
      attrs->attr_type = STATIC_ATTR;
      attrs->is_defined = (var_dclr->init != NULL);
      attrs->storage = STATIC;
      attrs->init.init_type = (var_dclr->init != NULL) ? INITIAL : TENTATIVE;
      attrs->init.init_list = init_list;
      attrs->cleanup_handler = NULL;
      symbol_table_insert(global_symbol_table, var_dclr->name, var_dclr->type, attrs);
    } else {
      // ensure the existing entry is not a function
      if (entry->type->type == FUN_TYPE) {
        type_error_at1_slice(var_dclr->name->start, "function %.*s redeclared as variable", (int)var_dclr->name->len, var_dclr->name->start);
        return false;
      }

      // ensure both declarations have the same type
      if (!compare_types(entry->type, var_dclr->type)) {
        type_error_at1_slice(var_dclr->name->start, "conflicting declarations for variable %.*s", (int)var_dclr->name->len, var_dclr->name->start);
        return false;
      }

      // check for duplicate definitions
      if (entry->attrs->is_defined && var_dclr->init != NULL) {
        type_error_at1_slice(var_dclr->name->start, "conflicting local static variable definitions for variable %.*s", (int)var_dclr->name->len, var_dclr->name->start);
        return false;
      }

      // update definition status
      if (var_dclr->init != NULL) {
        entry->attrs->is_defined = true;
        entry->attrs->init.init_type = INITIAL;
        entry->attrs->init.init_list = init_list;
      }
    }

    return true;
  } else {
    // Regular local variable must be unique within the current function scope.
    struct SymbolEntry* entry = symbol_table_get(global_symbol_table, var_dclr->name);
    if (entry != NULL) {
      type_error_at1_slice(var_dclr->name->start, "duplicate local variable declaration for variable %.*s", (int)var_dclr->name->len, var_dclr->name->start);
      return false;
    }

    // add to symbol table
    struct IdentAttr* attrs = arena_alloc(sizeof(struct IdentAttr));
    attrs->attr_type = LOCAL_ATTR;
    attrs->is_defined = true;
    attrs->storage = NONE; // local variables have no storage class
    attrs->cleanup_handler = var_dclr->attributes.cleanup_func;
    symbol_table_insert(global_symbol_table, var_dclr->name, var_dclr->type, attrs);

    if (var_dclr->init != NULL) {
      // Allow self-references in initializers (e.g., int a = a = 5).
      if (!typecheck_init(var_dclr->init, var_dclr->type)) {
        return false;
      }
    }
  }

  // this is a local declaration, so we should typecheck the cleanup if it exists
  if (var_dclr->attributes.cleanup_func != NULL) {
    // look up the cleanup function in the symbol table
    struct SymbolEntry* cleanup_entry = symbol_table_get(global_symbol_table, var_dclr->attributes.cleanup_func);
    if (cleanup_entry == NULL) {
      type_error_at2_slice(var_dclr->attributes.cleanup_func->start, "unknown cleanup function %.*s for static local variable %.*s", (int)var_dclr->attributes.cleanup_func->len, var_dclr->attributes.cleanup_func->start, (int)var_dclr->name->len, var_dclr->name->start);
      return false;
    }

    // ensure the cleanup function has the correct type: void func(type*)
    struct Type* cleanup_type = cleanup_entry->type;
    if (cleanup_type->type != FUN_TYPE ||
        cleanup_type->type_data.fun_type.return_type->type != VOID_TYPE ||
        cleanup_type->type_data.fun_type.param_types == NULL ||
        cleanup_type->type_data.fun_type.param_types->next != NULL) {
      type_error_at1_slice(var_dclr->attributes.cleanup_func->start, "cleanup function %.*s must have type void func(type*)", (int)var_dclr->attributes.cleanup_func->len, var_dclr->attributes.cleanup_func->start);
      return false;
    }

    // check parameter type
    struct Type* param_type = cleanup_type->type_data.fun_type.param_types->type;
    if (param_type->type != POINTER_TYPE ||
        !compare_types(param_type->type_data.pointer_type.referenced_type, var_dclr->type)) {
      type_error_at2_slice(var_dclr->attributes.cleanup_func->start, "cleanup function %.*s must have type void func(type*) where type matches variable %.*s", (int)var_dclr->attributes.cleanup_func->len, var_dclr->attributes.cleanup_func->start, (int)var_dclr->name->len, var_dclr->name->start);
      return false;
    }
  }

  return true;
}

struct Type* make_pointer_type(struct Type* type) {
  struct Type* ptr_type = arena_alloc(sizeof(struct Type));
  ptr_type->type = POINTER_TYPE;
  ptr_type->type_data.pointer_type.referenced_type = type;
  return ptr_type;
}

// Purpose: Typecheck an expression and apply conversion rules.
// Inputs: expr is the expression node.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Currently delegates to typecheck_expr.
bool typecheck_convert_expr(struct Expr** expr) {
  if (!typecheck_expr(*expr)) {
    return false;
  }
  if ((*expr)->value_type->type == ARRAY_TYPE) {
    struct Expr* addr_expr = arena_alloc(sizeof(struct Expr));
    addr_expr->type = ADDR_OF;
    addr_expr->loc = (*expr)->loc;
    addr_expr->expr.addr_of_expr.expr = *expr;
    addr_expr->value_type = make_pointer_type((*expr)->value_type->type_data.array_type.element_type);
    *expr = addr_expr;
  }
  if ((*expr)->value_type->type == FUN_TYPE) {
    // Function designators decay to pointers in expression contexts.
    struct Expr* addr_expr = arena_alloc(sizeof(struct Expr));
    addr_expr->type = ADDR_OF;
    addr_expr->loc = (*expr)->loc;
    addr_expr->expr.addr_of_expr.expr = *expr;
    addr_expr->value_type = make_pointer_type((*expr)->value_type);
    *expr = addr_expr;
  }
  if ((*expr)->value_type->type == STRUCT_TYPE ||
      (*expr)->value_type->type == UNION_TYPE) {
    // Struct/union values must be complete
    struct TypeEntry* entry = type_table_get(global_type_table,
                                              (*expr)->value_type->type_data.struct_type.name);
    if (entry == NULL) {
      type_error_at1_slice((*expr)->loc, "incomplete type for struct/union %.*s", (int)(*expr)->value_type->type_data.struct_type.name->len, (*expr)->value_type->type_data.struct_type.name->start);
      return false;
    }
  }
  return true;
}

bool typecheck_array_init(struct Initializer* init, struct Type* type) {
  struct InitializerList* cur_init = init->init.compound_init;
  struct InitializerList* prev_init = NULL;
  struct Type* element_type = type->type_data.array_type.element_type;
  size_t expected_elements = type->type_data.array_type.size;

  size_t count = 0;
  while (cur_init != NULL) {
    if (!typecheck_init(cur_init->init, element_type)) {
      return false;
    }
    count++;
    prev_init = cur_init;
    cur_init = cur_init->next;
  }

  if (count > expected_elements) {
    type_error_at2_size(init->loc, "too many initializers, got %zu, expected %zu", count, expected_elements);
    return false;
  }

  while (count < expected_elements) {
    // pad with zero initializers
    struct Initializer* zero_init = make_zero_initializer(element_type);
    struct InitializerList* new_init = arena_alloc(sizeof(struct InitializerList));
    new_init->init = zero_init;
    new_init->next = NULL;
    if (prev_init == NULL) {
      prev_init = new_init;
      init->init.compound_init = new_init;
    } else {
      prev_init->next = new_init;
      prev_init = new_init;
    }

    count++;
  }

  init->type = type;

  return true;
}

bool typecheck_struct_init(struct Initializer* init, struct Type* type) {
  struct InitializerList* cur_init = init->init.compound_init;
  struct InitializerList* prev_init = NULL;
  struct MemberEntry* member_entry = type_table_get(global_type_table, type->type_data.struct_type.name)->data.struct_entry->members;

  while (cur_init != NULL && member_entry != NULL) {
    if (!typecheck_init(cur_init->init, member_entry->type)) {
      return false;
    }
    prev_init = cur_init;
    cur_init = cur_init->next;
    member_entry = member_entry->next;
  }

  if (cur_init != NULL) {
    type_error_at1_slice(init->loc, "too many initializers for struct %.*s", (int)type->type_data.struct_type.name->len, type->type_data.struct_type.name->start);
    return false;
  }

  while (member_entry != NULL) {
    // pad with zero initializers
    struct Initializer* zero_init = make_zero_initializer(member_entry->type);
    struct InitializerList* new_init = arena_alloc(sizeof(struct InitializerList));
    new_init->init = zero_init;
    new_init->next = NULL;

    if (prev_init == NULL) {
      prev_init = new_init;
      init->init.compound_init = new_init;
    } else {
      prev_init->next = new_init;
      prev_init = new_init;
    }

    member_entry = member_entry->next;
  }

  init->type = type;

  return true;
}

bool typecheck_union_init(struct Initializer* init, struct Type* type) {
  struct InitializerList* cur_init = init->init.compound_init;

  if (cur_init == NULL) {
    type_error_at(init->loc, "union initializer requires at least one initializer");
    return false;
  }

  struct MemberEntry* member_entry = type_table_get(global_type_table, type->type_data.union_type.name)->data.union_entry->members;

  if (member_entry == NULL) {
    type_error_at(init->loc, "no matching member type found for union initializer");
    return false;
  }

  if (!typecheck_init(cur_init->init, member_entry->type)) {
    return false;
  }

  if (cur_init->next != NULL) {
    type_error_at1_slice(init->loc, "too many initializers for union %.*s", (int)type->type_data.union_type.name->len, type->type_data.union_type.name->start);
    return false;
  }

  init->type = type;

  return true;
}

// Purpose: Typecheck and convert an initializer expression.
// Inputs: init is the initializer pointer; type is the target type.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: May rewrite *init with a cast expression.
bool typecheck_init(struct Initializer* init, struct Type* type) {
  if (init == NULL) {
    return true; // Nothing to typecheck
  }

  switch (init->init_type) {
    case SINGLE_INIT: {
      if (type->type == ARRAY_TYPE && init->init.single_init->type == STRING) {
        // single string initializer for char array
        if (!is_char_type(type->type_data.array_type.element_type)) {
          type_error_at(init->loc, "string initializer requires char array type");
          return false;
        }
        init->init.single_init->value_type = type;
        if (init->init.single_init->expr.string_expr.string->len >
            type->type_data.array_type.size) {
          type_error_at(init->loc, "string initializer too long for array");
          return false;
        }
        init->type = type;
        return true;
      } else {
        // normal single initializer
        if (!typecheck_convert_expr(&init->init.single_init)) {
          return false;
        }

        if (!convert_by_assignment(&init->init.single_init, type)) {
          return false;
        }

        init->type = type;

        return true;
      }
    }
    case COMPOUND_INIT: {
      if (type->type == ARRAY_TYPE) {
        // array initializer
        return typecheck_array_init(init, type);
      } else if (type->type == STRUCT_TYPE) {
        // struct initializer
        return typecheck_struct_init(init, type);
      } else if (type->type == UNION_TYPE) {
        // union initializer
        return typecheck_union_init(init, type);
      }
      else {
        type_error_at(init->loc, "compound initializer requires array, struct, or union type");
        return false;
      }
    }
    default:
      type_error_at(NULL, "unknown initializer type in typecheck_init");
      return false; // Unknown initializer type
  }
}

struct Initializer* make_zero_initializer(struct Type* type) {
  struct Initializer* init = arena_alloc(sizeof(struct Initializer));
  init->loc = NULL;
  
  switch (type->type) {
    case CHAR_TYPE:
    case UCHAR_TYPE:
    case SCHAR_TYPE:
    case SHORT_TYPE:
    case USHORT_TYPE:
    case INT_TYPE:
    case UINT_TYPE:
    case POINTER_TYPE: {
      struct Expr* lit_expr = arena_alloc(sizeof(struct Expr));
      lit_expr->type = LIT;
      lit_expr->loc = NULL;
      lit_expr->expr.lit_expr.value.int_val = 0;
      lit_expr->value_type = type;
      lit_expr->expr.lit_expr.type = UINT_CONST;

      init->init_type = SINGLE_INIT;
      init->init.single_init = lit_expr;
      init->type = type;
      break;
    }
    case LONG_TYPE:
    case ULONG_TYPE:
      type_error_at(NULL,
                    "bootstrap bcc does not support long static initializers");
      return false;
    case ARRAY_TYPE: {
      init->init_type = COMPOUND_INIT;
      init->init.compound_init = NULL;

      size_t size = type->type_data.array_type.size;
      struct Type* element_type = type->type_data.array_type.element_type;

      struct InitializerList* prev_init = NULL;
      for (size_t i = 0; i < size; i++) {
        struct Initializer* elem_init = make_zero_initializer(element_type);
        struct InitializerList* new_init = arena_alloc(sizeof(struct InitializerList));
        new_init->init = elem_init;
        new_init->next = NULL;

        if (prev_init == NULL) {
          init->init.compound_init = new_init;
        } else {
          prev_init->next = new_init;
        }
        prev_init = new_init;
      }

      init->type = type;
      break;
    }
    // TODO: verify these
    case STRUCT_TYPE: {
      init->init_type = COMPOUND_INIT;
      init->init.compound_init = NULL;

      struct TypeEntry* type_entry = type_table_get(global_type_table, type->type_data.struct_type.name);
      struct MemberEntry* member_entry = type_entry->data.struct_entry->members;

      struct InitializerList* prev_init = NULL;
      while (member_entry != NULL) {
        struct Initializer* member_init = make_zero_initializer(member_entry->type);
        struct InitializerList* new_init = arena_alloc(sizeof(struct InitializerList));
        new_init->init = member_init;
        new_init->next = NULL;

        if (prev_init == NULL) {
          init->init.compound_init = new_init;
        } else {
          prev_init->next = new_init;
        }
        prev_init = new_init;

        member_entry = member_entry->next;
      }

      init->type = type;
      break;
    }
    case UNION_TYPE: {
      init->init_type = COMPOUND_INIT;
      init->init.compound_init = NULL;
      break;
    }
    case ENUM_TYPE: {
      struct Expr* lit_expr = arena_alloc(sizeof(struct Expr));
      lit_expr->type = LIT;
      lit_expr->loc = NULL;
      lit_expr->expr.lit_expr.value.int_val = 0;
      lit_expr->value_type = type;
      lit_expr->expr.lit_expr.type = UINT_CONST;

      init->init_type = SINGLE_INIT;
      init->init.single_init = lit_expr;
      init->type = type;
      break;
    }
    default:
      // Unsupported type for zero initializer
      type_error_at(NULL, "unsupported type for zero initializer");
      break;
  }

  return init;
}

// Purpose: Typecheck an expression subtree and set value_type.
// Inputs: expr is the expression node.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: value_type is assigned for each expression node.
bool typecheck_expr(struct Expr* expr) {
  switch (expr->type) {
    case BINARY: {
      struct BinaryExpr* bin_expr = &expr->expr.bin_expr;
      if (!typecheck_convert_expr(&bin_expr->left)) {
        return false;
      }
      if (!typecheck_convert_expr(&bin_expr->right)) {
        return false;
      }

      struct Type* left_type = bin_expr->left->value_type;
      struct Type* right_type = bin_expr->right->value_type;

      if (is_compound_assign_op(bin_expr->op)) {
        if (!is_assignable(bin_expr->left)) {
          type_error_at(expr->loc, "cannot assign to non-lvalue");
          return false;
        }

        enum BinOp base_op = compound_assign_base_op(bin_expr->op);
        if ((base_op == ADD_OP || base_op == SUB_OP) &&
            is_pointer_type(left_type) &&
            is_arithmetic_type(right_type)) {
          expr->value_type = left_type;
          return true;
        }

        if (!is_arithmetic_type(left_type) || !is_arithmetic_type(right_type)) {
          type_error_at(expr->loc, "invalid types in compound assignment");
          return false;
        }

        if (base_op == BIT_SHL || base_op == BIT_SHR) {
          convert_expr_type(&bin_expr->right, left_type);
          expr->value_type = left_type;
          return true;
        }

        struct Type* op_type = get_common_type(left_type, right_type);
        if (op_type == NULL) {
          type_error_at(expr->loc, "incompatible types in compound assignment");
          return false;
        }
        convert_expr_type(&bin_expr->right, op_type);
        expr->value_type = left_type;
        return true;
      }

      if (bin_expr->op == BOOL_EQ || bin_expr->op == BOOL_NEQ) {
        struct Type* common_type = NULL;
        if (is_pointer_type(left_type) || is_pointer_type(right_type)) {
          common_type = get_common_pointer_type(bin_expr->left, bin_expr->right);
          if (common_type == NULL) {
            type_error_at(expr->loc, "incompatible pointer types in equality comparison");
            return false;
          }
        } else if (is_arithmetic_type(left_type) && is_arithmetic_type(right_type)) {
          common_type = get_common_type(left_type, right_type);
          if (common_type == NULL) {
            type_error_at(expr->loc, "incompatible types in equality comparison");
            return false;
          }
        } else {
          type_error_at(expr->loc, "invalid types in equality comparison");
          return false;
        }

        convert_expr_type(&bin_expr->left, common_type);
        convert_expr_type(&bin_expr->right, common_type);
        expr->value_type = arena_alloc(sizeof(struct Type));
        expr->value_type->type = INT_TYPE; // result type of equality comparison is int
        return true;
      } else if (bin_expr->op == BOOL_LE || bin_expr->op == BOOL_LEQ ||
                 bin_expr->op == BOOL_GE || bin_expr->op == BOOL_GEQ) {
        if (is_pointer_type(left_type) || is_pointer_type(right_type)) {
          if (!is_pointer_type(left_type) || !is_pointer_type(right_type)) {
            type_error_at(expr->loc, "invalid types in relational comparison");
            return false;
          }
          struct Type* common_type = get_common_pointer_type(bin_expr->left, bin_expr->right);
          if (common_type == NULL) {
            type_error_at(expr->loc, "incompatible pointer types in relational comparison");
            return false;
          }
          convert_expr_type(&bin_expr->left, common_type);
          convert_expr_type(&bin_expr->right, common_type);
          expr->value_type = &kIntType;
          return true;
        }
        if (!is_arithmetic_type(left_type) || !is_arithmetic_type(right_type)) {
          type_error_at(expr->loc, "invalid types in relational comparison");
          return false;
        }
        struct Type* common_type = get_common_type(left_type, right_type);
        if (common_type == NULL) {
          type_error_at(expr->loc, "incompatible types in relational comparison");
          return false;
        }
        convert_expr_type(&bin_expr->left, common_type);
        convert_expr_type(&bin_expr->right, common_type);
        expr->value_type = &kIntType;
        return true;
      }
      else if (bin_expr->op == ADD_OP || bin_expr->op == PLUS_EQ_OP) {
        if (is_arithmetic_type(left_type) && is_arithmetic_type(right_type)) {
          struct Type* common_type = get_common_type(left_type, right_type);
          convert_expr_type(&bin_expr->left, common_type);
          convert_expr_type(&bin_expr->right, common_type);
          expr->value_type = common_type;
          return true;
        } else if ((is_arithmetic_type(left_type) && is_pointer_to_complete_type(right_type)) ||
                   (is_pointer_to_complete_type(left_type) && is_arithmetic_type(right_type))) {
          expr->value_type = is_pointer_type(left_type) ? left_type : right_type;
          return true;
        } else {
          type_error_at(expr->loc, "invalid types for pointer arithmetic in addition");
          return false;
        }
      }
      else if (bin_expr->op == SUB_OP || bin_expr->op == MINUS_EQ_OP) {
        if (is_arithmetic_type(left_type) && is_arithmetic_type(right_type)) {
          struct Type* common_type = get_common_type(left_type, right_type);
          convert_expr_type(&bin_expr->left, common_type);
          convert_expr_type(&bin_expr->right, common_type);
          expr->value_type = common_type;
          return true;
        } else if (is_pointer_to_complete_type(left_type) && is_arithmetic_type(right_type)) {
          // Pointer minus integer yields a pointer.
          expr->value_type = left_type;
          return true;
        } else {
          type_error_at(expr->loc, "invalid types for pointer arithmetic in subtraction");
          return false;
        }
      } 
      else if (bin_expr->op == BOOL_AND || bin_expr->op == BOOL_OR) {
        // Logical operators always yield int in this language subset.
        if (!is_scalar_type(left_type) || !is_scalar_type(right_type)) {
          type_error_at(expr->loc, "invalid types in logical operation");
          return false;
        }
        expr->value_type = &kIntType; // result type of logical operations is int
        return true;
      }
      else if (bin_expr->op == BIT_SHL || bin_expr->op == BIT_SHR) {
        if (!is_arithmetic_type(left_type) || !is_arithmetic_type(right_type)) {
          type_error_at(expr->loc, "invalid types in shift expression");
          return false;
        }
        convert_expr_type(&bin_expr->right, left_type);
        expr->value_type = left_type;
        return true;
      } else if (bin_expr->op == COMMA_OP){
        // The result type of the comma operator is the type of the right operand.
        expr->value_type = right_type;
        return true;
      } else {
        if (!is_arithmetic_type(left_type) || !is_arithmetic_type(right_type)) {
          type_error_at(expr->loc, "invalid types in binary operation");
          return false;
        }
        struct Type* common_type = get_common_type(left_type, right_type);
        if (common_type == NULL) {
          type_error_at(expr->loc, "incompatible types in binary operation");
          return false;
        }
        convert_expr_type(&bin_expr->left, common_type);
        convert_expr_type(&bin_expr->right, common_type);
        expr->value_type = common_type;
        
        return true;
      }
    }
    case ASSIGN: {
      struct AssignExpr* assign_expr = &expr->expr.assign_expr;
      if (!typecheck_convert_expr(&assign_expr->left)) {
        return false;
      }

      if (!is_assignable(assign_expr->left)) {
        type_error_at(expr->loc, "cannot assign to non-lvalue");
        return false;
      }

      if (!typecheck_convert_expr(&assign_expr->right)) {
        return false;
      }

      if (!convert_by_assignment(&assign_expr->right, assign_expr->left->value_type)) {
        type_error_at(expr->loc, "incompatible types in assignment");
        return false;
      }

      expr->value_type = assign_expr->left->value_type;
      return true;
    }
    case POST_ASSIGN: {
      struct PostAssignExpr* post_assign_expr = &expr->expr.post_assign_expr;
      if (!typecheck_convert_expr(&post_assign_expr->expr)) {
        return false;
      }

      if (!is_assignable(post_assign_expr->expr)) {
        type_error_at(expr->loc, "cannot apply post-increment/decrement to non-lvalue");
        return false;
      }

      if (!is_arithmetic_type(post_assign_expr->expr->value_type) &&
          !is_pointer_type(post_assign_expr->expr->value_type)) {
        type_error_at(expr->loc,
                      "post-increment/decrement requires arithmetic or pointer type");
        return false;
      }

      expr->value_type = post_assign_expr->expr->value_type;
      return true;
    }
    case CONDITIONAL: {
      struct ConditionalExpr* cond_expr = &expr->expr.conditional_expr;
      if (!typecheck_convert_expr(&cond_expr->condition)) {
        return false;
      }
      if (!typecheck_convert_expr(&cond_expr->left)) {
        return false;
      }
      if (!typecheck_convert_expr(&cond_expr->right)) {
        return false;
      }

      if (cond_expr->left->value_type->type == STRUCT_TYPE ||
          cond_expr->right->value_type->type == STRUCT_TYPE){
        // both must be struct type and identical
        if (!compare_types(cond_expr->left->value_type, cond_expr->right->value_type)) {
          type_error_at(expr->loc, "incompatible struct types in conditional expression");
          return false;
        }
        expr->value_type = cond_expr->left->value_type;
        return true;
      }

      if (cond_expr->left->value_type->type == UNION_TYPE ||
          cond_expr->right->value_type->type == UNION_TYPE){
        // both must be union type and identical
        if (!compare_types(cond_expr->left->value_type, cond_expr->right->value_type)) {
          type_error_at(expr->loc, "incompatible struct types in conditional expression");
          return false;
        }
        expr->value_type = cond_expr->left->value_type;
        return true;
      }

      if (!is_scalar_type(cond_expr->condition->value_type)) {
        type_error_at(expr->loc, "condition in conditional expression must have scalar type");
        return false;
      }

      struct Type* left_type = cond_expr->left->value_type;
      struct Type* right_type = cond_expr->right->value_type;

      // Find a compatible common type for the true/false arms.
      struct Type* common_type = NULL;
      if (left_type->type == VOID_TYPE && right_type->type == VOID_TYPE) {
        expr->value_type = &kVoidType;
        return true;
      } else if (is_arithmetic_type(left_type) && is_arithmetic_type(right_type)) {
        common_type = get_common_type(left_type, right_type);
        if (common_type == NULL) {
          type_error_at(expr->loc, "incompatible arithmetic types in conditional expression");
          return false;
        }
      } else if (is_pointer_type(left_type) || is_pointer_type(right_type)) {
        common_type = get_common_pointer_type(cond_expr->left, cond_expr->right);
        if (common_type == NULL) {
          type_error_at(expr->loc, "incompatible pointer types in conditional expression");
          return false;
        }
      } else {
        type_error_at(expr->loc, "incompatible types in conditional expression");
        return false;
      }

      convert_expr_type(&cond_expr->left, common_type);
      convert_expr_type(&cond_expr->right, common_type);
      expr->value_type = common_type;
      return true;
    }
    case FUNCTION_CALL: {
      struct Expr* func_expr = expr->expr.fun_call_expr.func;
      struct Type* func_type = NULL;
      if (func_expr->type == VAR) {
        // Identifiers can refer to functions or function pointers.
        struct SymbolEntry* entry = symbol_table_get(global_symbol_table,
                                                     func_expr->expr.var_expr.name);
        if (entry == NULL) {
          type_error_at(expr->loc, "unknown function in function call");
          return false;
        }
        func_type = entry->type;
      } else {
        if (!typecheck_convert_expr(&expr->expr.fun_call_expr.func)) {
          return false;
        }
        func_expr = expr->expr.fun_call_expr.func;
        func_type = func_expr->value_type;
      }

      if (func_type->type == POINTER_TYPE &&
          func_type->type_data.pointer_type.referenced_type->type == FUN_TYPE) {
        func_type = func_type->type_data.pointer_type.referenced_type;
      }
      if (func_type->type != FUN_TYPE) {
        if (func_expr->type == VAR) {
          type_error_at1_slice(expr->loc, "variable %.*s cannot be used as a function", (int)func_expr->expr.var_expr.name->len, func_expr->expr.var_expr.name->start);
        } else {
          type_error_at(expr->loc, "expression cannot be used as a function");
        }
        return false;
      }

      // Arguments are converted by assignment to each parameter type.
      struct ParamTypeList* param_types = func_type->type_data.fun_type.param_types;
      if (!typecheck_args(expr->expr.fun_call_expr.args, param_types, expr)) {
        return false;
      }
      expr->value_type = func_type->type_data.fun_type.return_type;
      return true;
    }
    case VAR: {
      struct SymbolEntry* entry = symbol_table_get(global_symbol_table, expr->expr.var_expr.name);
      if (entry == NULL) {
        type_error_at1_slice(expr->loc, "unknown variable %.*s", (int)expr->expr.var_expr.name->len, expr->expr.var_expr.name->start);
        return false;
      }
      expr->value_type = entry->type;
      return true;
    }
    case UNARY: {
      struct UnaryExpr* unary_expr = &expr->expr.un_expr;
      if (!typecheck_convert_expr(&unary_expr->expr)) {
        return false;
      }
      struct Type* expr_type = unary_expr->expr->value_type;
      if (is_pointer_type(expr_type) &&
          (unary_expr->op == NEGATE || unary_expr->op == COMPLEMENT)) {
        type_error_at(expr->loc, "invalid pointer operation in unary expression");
        return false;
      }
      if (is_char_type(expr_type) &&
          (unary_expr->op == NEGATE || unary_expr->op == COMPLEMENT || unary_expr->op == UNARY_PLUS)) {
        //  Promote char to int for these operations.
        convert_expr_type(&unary_expr->expr, &kIntType);
        expr_type = unary_expr->expr->value_type;
      }
      if (unary_expr->op == BOOL_NOT) {
        if (!is_scalar_type(expr_type)) {
          type_error_at(expr->loc, "logical not requires scalar type");
          return false;
        }
        expr->value_type = &kIntType;
      } else {
        expr->value_type = expr_type;
      }
      return true;
    }
    case LIT: {
      struct LitExpr* lit_expr = &expr->expr.lit_expr;
      expr->value_type = arena_alloc(sizeof(struct Type));
      switch (lit_expr->type) {
        case INT_CONST:
          expr->value_type->type = INT_TYPE;
          return true;
        case UINT_CONST:
          expr->value_type->type = UINT_TYPE;
          return true;
        case LONG_CONST:
        case ULONG_CONST:
          type_error_at(expr->loc,
                        "bootstrap bcc does not support long integer literals");
          return false;
        default:
          type_error_at(expr->loc, "unknown literal type in typecheck_expr");
          return false; // Unknown literal type
      }
    }
    case CAST: {
      struct CastExpr* cast_expr = &expr->expr.cast_expr;
    
      if (!typecheck_convert_expr(&cast_expr->expr)) {
        return false;
      }

      if (cast_expr->target->type == VOID_TYPE) {
        // can always cast to void
        expr->value_type = cast_expr->target;
        return true;
      }

      if (!is_scalar_type(cast_expr->target)){
        type_error_at(expr->loc, "can only cast to scalar types or void");
        return false;
      }

      if (!is_scalar_type(cast_expr->expr->value_type)) {
        type_error_at(expr->loc, "cannot cast non-scalar type to scalar type");
        return false;
      }

      if (!is_valid_type_specifier(cast_expr->target)) {
        type_error_at(expr->loc, "invalid target type in cast expression");
        return false;
      }

      expr->value_type = cast_expr->target;
      return true;
    }
    case ADDR_OF: {
      struct AddrOfExpr* addr_of_expr = &expr->expr.addr_of_expr;
      if (!typecheck_expr(addr_of_expr->expr)) {
        return false;
      }
      if (!is_lvalue(addr_of_expr->expr)) {
        type_error_at(expr->loc, "cannot take the address of a non-lvalue");
        return false;
      }
      struct Type* referenced = addr_of_expr->expr->value_type;
      expr->value_type = arena_alloc(sizeof(struct Type));
      expr->value_type->type = POINTER_TYPE;
      expr->value_type->type_data.pointer_type.referenced_type = referenced;
      return true;
    }
    case DEREFERENCE: {
      struct DereferenceExpr* deref_expr = &expr->expr.deref_expr;
      if (!typecheck_convert_expr(&deref_expr->expr)) {
        return false;
      }
      struct Type* expr_type = deref_expr->expr->value_type;
      if (!is_pointer_type(expr_type)) {
        type_error_at(expr->loc, "cannot dereference non-pointer type");
        return false;
      }
      expr->value_type = expr_type->type_data.pointer_type.referenced_type;
      return true;
    }
    case SUBSCRIPT: {
      struct SubscriptExpr* sub_expr = &expr->expr.subscript_expr;
      if (!typecheck_convert_expr(&sub_expr->array)) {
        return false;
      }
      if (!typecheck_convert_expr(&sub_expr->index)) {
        return false;
      }
      struct Type* array_type = sub_expr->array->value_type;
      struct Type* index_type = sub_expr->index->value_type;

      struct Type* ptr_type = NULL;
      if (is_pointer_type(array_type) && is_arithmetic_type(index_type)) {
        // valid
        ptr_type = array_type;
        convert_expr_type(&sub_expr->index, &kIntType);
      } else if (is_arithmetic_type(array_type) && is_pointer_type(index_type)) {
        // valid
        ptr_type = index_type;
        convert_expr_type(&sub_expr->array, &kIntType);

        // swap array and index
        struct Expr* temp = sub_expr->array;
        sub_expr->array = sub_expr->index;
        sub_expr->index = temp;
      } else {
        type_error_at(expr->loc, "array subscript requires array/pointer and integer types");
        return false;
      }

      if (!is_arithmetic_type(index_type)) {
        type_error_at(expr->loc, "array subscript is not an integer type");
        return false;
      }

      expr->value_type = ptr_type->type_data.pointer_type.referenced_type;
      return true;
    }
    case STRING: {
      struct StringExpr* str_expr = &expr->expr.string_expr;
      expr->value_type = arena_alloc(sizeof(struct Type));
      expr->value_type->type = ARRAY_TYPE;
      expr->value_type->type_data.array_type.size = str_expr->string->len + 1; // include null terminator
      expr->value_type->type_data.array_type.element_type = &kCharType;
      return true;
    }
    case SIZEOF_EXPR: {
      if (!typecheck_expr(expr->expr.sizeof_expr.expr)) {
        return false;
      }
      if (!is_complete_type(expr->expr.sizeof_expr.expr->value_type)) {
        type_error_at(expr->loc, "incomplete type in sizeof expression");
        return false;
      }
      expr->value_type = &kUIntType; // not size_t yet, uint is large enough for now
      return true;
    }
    case SIZEOF_T_EXPR: {
      if (!is_valid_type_specifier(expr->expr.sizeof_t_expr.type)) {
        type_error_at(expr->loc, "invalid type in sizeof expression");
        return false;
      }
      if (!is_complete_type(expr->expr.sizeof_t_expr.type)) {
        type_error_at(expr->loc, "incomplete type in sizeof expression");
        return false;
      }
      expr->value_type = &kUIntType; // not size_t yet, uint is large enough for now
      return true;
    }
    case STMT_EXPR: {
      struct StmtExpr* stmt_expr = &expr->expr.stmt_expr;
      if (!typecheck_block(stmt_expr->block)) {
        return false;
      }

      // get the last statement from the block
      struct Block* last_item = stmt_expr->block;
      if (last_item == NULL) {
        type_error_at(expr->loc, "empty statement expression");
        return false;
      }
      while (last_item->next != NULL) {
        last_item = last_item->next;
      }

      if (last_item->item->type == STMT_ITEM && last_item->item->item.stmt->type == EXPR_STMT) {
        expr->value_type = last_item->item->item.stmt->statement.expr_stmt.expr->value_type;
      } else {
        expr->value_type = &kVoidType;
      }
      return true;
    }
    case DOT_EXPR: {
      if (!typecheck_convert_expr(&expr->expr.dot_expr.struct_expr)) {
        return false;
      }
      struct Type* struct_type = expr->expr.dot_expr.struct_expr->value_type;
      if (struct_type->type != STRUCT_TYPE && struct_type->type != UNION_TYPE) {
        type_error_at(expr->loc, "dot operator requires struct or union type");
        return false;
      }
      struct Slice* type_name = NULL;
      if (struct_type->type == STRUCT_TYPE) {
        type_name = struct_type->type_data.struct_type.name;
      } else {
        type_name = struct_type->type_data.union_type.name;
      }
      struct TypeEntry* type_entry = type_table_get(global_type_table, type_name);
      if (type_entry == NULL) {
        type_error_at(expr->loc, "incomplete struct/union type in dot expression");
        return false;
      }

      struct MemberEntry* member = NULL;
      if (struct_type->type == STRUCT_TYPE) {
        member = type_entry->data.struct_entry->members;
      } else {
        member = type_entry->data.union_entry->members;
      }
      while (member != NULL) {
        if (compare_slice_to_slice(member->key, expr->expr.dot_expr.member)) {
          expr->value_type = member->type;
          return true;   
        }
        member = member->next;
      }
      type_error_at(expr->loc, "no such member in struct/union for dot expression");
      return false;
    }
    case ARROW_EXPR: {
      if (!typecheck_convert_expr(&expr->expr.arrow_expr.pointer_expr)) {
        return false;
      }
      struct Type* ptr_type = expr->expr.arrow_expr.pointer_expr->value_type;
      if (!is_pointer_type(ptr_type)) {
        type_error_at(expr->loc, "arrow operator requires pointer type");
        return false;
      }
      struct Type* struct_type = ptr_type->type_data.pointer_type.referenced_type;
      if (struct_type->type != STRUCT_TYPE && struct_type->type != UNION_TYPE) {
        type_error_at(expr->loc, "arrow operator requires pointer to struct or union type");
        return false;
      }
      struct Slice* type_name = NULL;
      if (struct_type->type == STRUCT_TYPE) {
        type_name = struct_type->type_data.struct_type.name;
      } else {
        type_name = struct_type->type_data.union_type.name;
      }
      struct TypeEntry* type_entry = type_table_get(global_type_table, type_name);
      if (type_entry == NULL) {
        type_error_at(expr->loc, "incomplete struct/union type in arrow expression");
        return false;
      }

      struct MemberEntry* member = NULL;
      if (struct_type->type == STRUCT_TYPE) {
        member = type_entry->data.struct_entry->members;
      } else {
        member = type_entry->data.union_entry->members;
      }
      while (member != NULL) {
        if (compare_slice_to_slice(member->key, expr->expr.arrow_expr.member)) {
          expr->value_type = member->type;
          return true;   
        }
        member = member->next;
      }
      type_error_at(expr->loc, "no such member in struct/union for arrow expression");
      return false;
    }
    default: {
      type_error_at(expr->loc, "unknown expression type in typecheck_expr");
      return false; // Unknown expression type
    }
  }
}

// Purpose: Typecheck a function call argument list.
// Inputs: args are call arguments; types are parameter types; call_site for errors.
// Outputs: Returns true on success; false on any type error.
// Invariants/Assumptions: Arguments are converted by assignment.
bool typecheck_args(struct ArgList* args, struct ParamTypeList* types, struct Expr* call_site) {
  for (; args != NULL && types != NULL; args = args->next, types = types->next) {
    if (!typecheck_convert_expr(&args->arg)) {
      return false;
    }
    if (!convert_by_assignment(&args->arg, types->type)) {
      return false;
    }
  }
  if (args != NULL || types != NULL) {
    type_error_at(call_site ? call_site->loc : NULL,
                  "argument and parameter count mismatch");
    return false;
  }
  return true;
}

// ------------------------- Type Utility Functions ------------------------- //

// Purpose: Check if a type is arithmetic.
// Inputs: type is the Type node.
// Outputs: Returns true for integer-like types.
// Invariants/Assumptions: Pointer and function types are not arithmetic.
bool is_arithmetic_type(struct Type* type) {
  switch (type->type) {
    case INT_TYPE:
    case UINT_TYPE:
    case LONG_TYPE:
    case ULONG_TYPE:
    case SHORT_TYPE:
    case USHORT_TYPE:
    case CHAR_TYPE:
    case SCHAR_TYPE:
    case UCHAR_TYPE:
    case ENUM_TYPE: 
      return true;
    default:
      return false;
  }
}

bool is_unsigned_type(struct Type* type) {
  switch (type->type) {
    case UINT_TYPE:
    case ULONG_TYPE:
    case USHORT_TYPE:
    case POINTER_TYPE:
    case UCHAR_TYPE:
    case UNION_TYPE:
    case ENUM_TYPE:
      return true;
    default:
      return false;
  }
}

bool is_scalar_type(struct Type* type) {
  switch (type->type) {
    case VOID_TYPE:
    case ARRAY_TYPE:
    case FUN_TYPE:
    case STRUCT_TYPE:
    case UNION_TYPE:
      return false;
    default:
      return true;
  }
}

// Purpose: Check if a type is signed.
// Inputs: type is the Type node.
// Outputs: Returns true for signed integer types.
// Invariants/Assumptions: Unsigned types return false.
bool is_signed_type(struct Type* type) {
  switch (type->type) {
    case INT_TYPE:
    case LONG_TYPE:
    case SHORT_TYPE:
    case SCHAR_TYPE:
    case CHAR_TYPE:
      return true;
    default:
      return false;
  }
}

// Purpose: Check if a type is a pointer type.
// Inputs: type is the Type node.
// Outputs: Returns true if type->type == POINTER_TYPE.
// Invariants/Assumptions: Does not inspect referenced type.
bool is_pointer_type(struct Type* type) {
  return type->type == POINTER_TYPE;
}

bool is_char_type(struct Type* type) {
  return type->type == CHAR_TYPE || type->type == UCHAR_TYPE || type->type == SCHAR_TYPE;
}

// Purpose: Insert a cast expression to convert to a target type.
// Inputs: expr is the expression pointer; target is the desired type.
// Outputs: Rewrites *expr if a cast is needed.
// Invariants/Assumptions: Uses arena allocation for the new cast node.
void convert_expr_type(struct Expr** expr, struct Type* target) {
  if (!compare_types((*expr)->value_type, target)) {
    struct Expr* new_expr = arena_alloc(sizeof(struct Expr));
    new_expr->loc = (*expr)->loc;
    new_expr->type = CAST;
    new_expr->expr.cast_expr.target = target;
    new_expr->expr.cast_expr.expr = *expr;
    new_expr->value_type = target;
    *expr = new_expr;
  }
}

// Purpose: Compute the size of a type in bytes.
// Inputs: type is the Type node.
// Outputs: Returns the size in bytes or 0 for unknown types.
// Invariants/Assumptions: Pointer size is treated as 4 bytes here.
size_t get_type_size(struct Type* type) {
  switch (type->type) {
    case CHAR_TYPE:
    case UCHAR_TYPE:
    case SCHAR_TYPE:
      return 1;
    case SHORT_TYPE:
    case USHORT_TYPE:
      return 2;
    case INT_TYPE:
    case UINT_TYPE:
      return 4;
    case LONG_TYPE:
    case ULONG_TYPE:
      return 8;
    case POINTER_TYPE:
      return 4; // 32-bit architecture
    case ARRAY_TYPE:
      return type->type_data.array_type.size *
             get_type_size(type->type_data.array_type.element_type);
    case STRUCT_TYPE: {
      struct TypeEntry* entry = type_table_get(global_type_table, type->type_data.struct_type.name);
      if (entry == NULL) return -1; // incomplete type (don't error because sometimes this is valid)
      return entry->data.struct_entry->size;
    }
    case UNION_TYPE: {
      struct TypeEntry* entry = type_table_get(global_type_table, type->type_data.union_type.name);
      if (entry == NULL) return -1; // incomplete type
      return entry->data.union_entry->size;
    }
    case ENUM_TYPE: {
      return 4; // enums are treated as int
    }
    case VOID_TYPE: {
      type_error_at(NULL, "cannot get size of void type");
      return -1;
    }
    default:
      type_error_at(NULL, "unknown type size in get_type_size");
      return -1; // unknown type size
  }
}

size_t get_type_alignment(struct Type* type) {
  switch (type->type){
    case ARRAY_TYPE:
      return get_type_alignment(type->type_data.array_type.element_type);
    case STRUCT_TYPE: {
      struct TypeEntry* entry = type_table_get(global_type_table, type->type_data.struct_type.name);
      if (entry == NULL) return -1; // incomplete type (don't error because sometimes this is valid)
      return entry->data.struct_entry->alignment;
    }
    case UNION_TYPE: {
      struct TypeEntry* entry = type_table_get(global_type_table, type->type_data.union_type.name);
      if (entry == NULL) return -1; // incomplete type
      return entry->data.union_entry->alignment;
    }
    case ENUM_TYPE:
      return 4; // enums are treated as int
    case VOID_TYPE: {
      type_error_at(NULL, "cannot get alignment of void type");
      return -1;
    }
    default: {
      size_t size = get_type_size(type);
      if (size >= 4) {
        return 4;
      } else if (size == 3 || size == 0) {
        // 1) what
        type_error_at(NULL, "invalid type size in get_type_alignment");
        return -1;
      } else {
        return size;
      }
    }
  }
}

// Purpose: Determine the common arithmetic type of two operands.
// Inputs: t1 and t2 are operand types.
// Outputs: Returns a common type or NULL if incompatible.
// Invariants/Assumptions: Pointer types are not handled here.
struct Type* get_common_type(struct Type* t1, struct Type* t2) {
  // promote char types to int
  if (is_char_type(t1)) {
    t1 = &kIntType;
  }
  if (is_char_type(t2)) {
    t2 = &kIntType;
  }

  if (compare_types(t1, t2)) {
    return t1;
  }

  size_t size1 = get_type_size(t1);
  size_t size2 = get_type_size(t2);

  if (size1 == size2) {
    if (is_signed_type(t1)) {
      return t2;
    } else {
      return t1;
    }
  } else if (size1 > size2) {
    return t1;
  } else {
    return t2;
  }
}

// Purpose: Determine if an expression is an lvalue
bool is_lvalue(struct Expr* expr) {
  switch (expr->type) {
    case VAR:
      return true;
    case DEREFERENCE:
      return true;
    case SUBSCRIPT:
      return true;
    case STRING:
      return true; // string literals can be treated as lvalues for address-of
    case DOT_EXPR:
      return is_lvalue(expr->expr.dot_expr.struct_expr);
    case ARROW_EXPR:
      return true;
    default:
      return false;
  }
}

bool is_assignable(struct Expr* expr) {
  if (expr->type == STRING) {
    return false; // string literals are not assignable, but are still lvalues
  }
  return is_lvalue(expr);
}

// Purpose: Check if an expression is a null pointer constant.
// Inputs: expr is the expression node.
// Outputs: Returns true for literal integer 0.
// Invariants/Assumptions: Only INT_CONST zero is treated as null.
bool is_null_pointer_constant(struct Expr* expr) {
  if (expr->type == LIT) {
    struct LitExpr* lit_expr = &expr->expr.lit_expr;
    if (lit_expr->type == INT_CONST &&
        lit_expr->value.int_val == 0) {
      return true;
    }
  }
  return false;
}

bool is_void_pointer_type(struct Type* type) {
  if (type->type != POINTER_TYPE) {
    return false;
  }
  struct Type* referenced = type->type_data.pointer_type.referenced_type;
  return referenced->type == VOID_TYPE;
}

bool is_complete_type(struct Type* type) {
  switch (type->type) {
    case ARRAY_TYPE:
      return is_complete_type(type->type_data.array_type.element_type);
    case STRUCT_TYPE: {
      struct TypeEntry* entry = type_table_get(global_type_table, type->type_data.struct_type.name);
      return entry != NULL;
    }
    case UNION_TYPE: {
      struct TypeEntry* entry = type_table_get(global_type_table, type->type_data.union_type.name);
      return entry != NULL;
    }
    case FUN_TYPE:
    case VOID_TYPE:
      return false;
    default:
      return true;
  }
}

bool is_pointer_to_complete_type(struct Type* type) {
  if (!is_pointer_type(type)) {
    return false;
  }
  struct Type* referenced = type->type_data.pointer_type.referenced_type;
  return is_complete_type(referenced);
}

bool is_valid_type_specifier(struct Type* type) {
  switch (type->type) {
    case ARRAY_TYPE: {
      // element type must be complete
      if (!is_complete_type(type->type_data.array_type.element_type)) {
        return false;
      }
      // recursively check element type
      return is_valid_type_specifier(type->type_data.array_type.element_type);
    }
    case FUN_TYPE: {
      // check parameter types and return type
      for (struct ParamTypeList* param = type->type_data.fun_type.param_types;
           param != NULL; param = param->next) {
        if (!is_valid_type_specifier(param->type)) {
          return false;
        }
      }
      return is_valid_type_specifier(type->type_data.fun_type.return_type);
    }
    case POINTER_TYPE: {
      // recursively check referenced type
      return is_valid_type_specifier(type->type_data.pointer_type.referenced_type);
    }
    default:
      return true;
  }
}

// Purpose: Determine a common pointer type for pointer comparisons/conditionals.
// Inputs: expr1 and expr2 are the operand expressions.
// Outputs: Returns a compatible pointer type or NULL if incompatible.
// Invariants/Assumptions: Allows null pointer constants to match any pointer.
struct Type* get_common_pointer_type(struct Expr* expr1, struct Expr* expr2) {
  struct Type* t1 = expr1->value_type;
  struct Type* t2 = expr2->value_type;

  if (compare_types(t1, t2)) {
    return t1;
  } else if (is_null_pointer_constant(expr1)) {
    return t2;
  } else if (is_null_pointer_constant(expr2)) {
    return t1;
  }
  return NULL;
}

// Purpose: Apply assignment conversion rules to an expression.
// Inputs: expr is the expression pointer; target is the target type.
// Outputs: Returns true on success; false on invalid conversions.
// Invariants/Assumptions: May rewrite *expr with a cast expression.
bool convert_by_assignment(struct Expr** expr, struct Type* target) {
  if (compare_types((*expr)->value_type, target)) {
    return true;
  }

  // Apply the assignment conversion rules for arithmetic and null-pointer cases.
  if (is_arithmetic_type((*expr)->value_type) && is_arithmetic_type(target)) {
    // perform conversion
    convert_expr_type(expr, target);
    return true;
  }

  if (is_pointer_type(target) && is_null_pointer_constant(*expr)) {
    // perform conversion (TODO: should really only allow this for null pointer constants)
    convert_expr_type(expr, target);
    return true;
  }

  if (is_void_pointer_type(target) && is_pointer_type((*expr)->value_type)) {
    // allow conversion to void* from other pointer types
    convert_expr_type(expr, target);
    return true;
  }

  if (is_pointer_type(target) && is_void_pointer_type((*expr)->value_type)) {
    // allow conversion from void* to other pointer types
    convert_expr_type(expr, target);
    return true;
  }

  type_error_at((*expr)->loc, "cannot convert type for assignment");
  return false;
}

// Purpose: Map a variable declaration to a static initializer kind.
// Inputs: var_dclr is the variable declaration node.
// Outputs: Returns a StaticInitType enum value.
// Invariants/Assumptions: Only integer-like types are supported here.
enum StaticInitType get_var_init(struct Type* type) {
  switch (type->type) {
    case CHAR_TYPE:
    case SCHAR_TYPE:
      return CHAR_INIT;
    case UCHAR_TYPE:
      return UCHAR_INIT;
    case SHORT_TYPE:
      return SHORT_INIT;
    case USHORT_TYPE:
      return USHORT_INIT;
    case INT_TYPE:
      return INT_INIT;
    case UINT_TYPE:
    case POINTER_TYPE:
    case ENUM_TYPE:
      return UINT_INIT;
    case LONG_TYPE:
      return LONG_INIT;
    case ULONG_TYPE:
      return ULONG_INIT;
    default:
      fdputs(STDOUT, "Warning: Unsupported variable type for static initialization\n");
      return -1; // unknown init type
  }
}


// ------------------------- Symbol Table Functions ------------------------- //

// Purpose: Allocate a symbol table with a given bucket count.
// Inputs: numBuckets is the number of hash buckets.
// Outputs: Returns a SymbolTable allocated in the arena.
// Invariants/Assumptions: Entries are arena-allocated and persist for the pass.
struct SymbolTable* create_symbol_table(size_t numBuckets){
  struct SymbolTable* table = arena_alloc(sizeof(struct SymbolTable));
  table->size = numBuckets;
  table->arr = arena_alloc(sizeof(struct SymbolEntry*) * numBuckets);
  for (size_t i = 0; i < numBuckets; i++){
    table->arr[i] = NULL;
  }
  return table;
}

// Purpose: Insert a symbol entry into the table.
// Inputs: hmap is the table; key/type/attrs define the symbol.
// Outputs: Updates the table in place.
// Invariants/Assumptions: Does not check for duplicates.
void symbol_table_insert(struct SymbolTable* hmap, struct Slice* key, struct Type* type, struct IdentAttr* attrs){
  size_t label = hash_slice(key) % hmap->size;
  
  struct SymbolEntry* newEntry = arena_alloc(sizeof(struct SymbolEntry));
  newEntry->key = key;
  newEntry->type = type;
  newEntry->attrs = attrs;
  newEntry->next = NULL;

  if (hmap->arr[label] == NULL){
    hmap->arr[label] = newEntry;
  } else {
    struct SymbolEntry* cur = hmap->arr[label];
    while (cur->next != NULL){
      cur = cur->next;
    }
    cur->next = newEntry;
  }
}

// Purpose: Look up a symbol entry by identifier name.
// Inputs: hmap is the table; key is the identifier slice.
// Outputs: Returns the entry or NULL if missing.
// Invariants/Assumptions: hash_slice is consistent with insertions.
struct SymbolEntry* symbol_table_get(struct SymbolTable* hmap, struct Slice* key){
  size_t label = hash_slice(key) % hmap->size;

  struct SymbolEntry* cur = hmap->arr[label];
  while (cur != NULL){
    if (compare_slice_to_slice(cur->key, key)){
      return cur;
    }
    cur = cur->next;
  }
  return NULL;
}

// Purpose: Check if a symbol exists in the table.
// Inputs: hmap is the table; key is the identifier slice.
// Outputs: Returns true if the symbol is present.
// Invariants/Assumptions: Performs a full lookup in the bucket chain.
bool symbol_table_contains(struct SymbolTable* hmap, struct Slice* key){
  size_t label = hash_slice(key) % hmap->size;

  struct SymbolEntry* cur = hmap->arr[label];
  while (cur != NULL){
    if (compare_slice_to_slice(cur->key, key)){
      return true;
    }
    cur = cur->next;
  }
  return false;
}

// Purpose: Print the symbol table contents for debugging.
// Inputs: hmap is the table to print.
// Outputs: Writes a human-readable dump to stdout.
// Invariants/Assumptions: Intended for debugging only.
void print_symbol_table(struct SymbolTable* hmap){
  (void)hmap;
}

// ------------------------- Type Table Functions ------------------------- //

// Purpose: Allocate a type table with a given bucket count.
// Inputs: numBuckets is the number of hash buckets.
// Outputs: Returns a TypeTable allocated in the arena.
// Invariants/Assumptions: Entries are arena-allocated and persist for the pass.
struct TypeTable* create_type_table(size_t numBuckets){
  struct TypeTable* table = arena_alloc(sizeof(struct TypeTable));
  table->size = numBuckets;
  table->arr = arena_alloc(sizeof(struct TypeEntry*) * numBuckets);
  for (size_t i = 0; i < numBuckets; i++){
    table->arr[i] = NULL;
  }
  return table;
}

// Purpose: Insert a type entry into the table.
// Inputs: hmap is the table; key/type/data define the entry.
// Outputs: Updates the table in place.
// Invariants/Assumptions: Does not check for duplicates.
void type_table_insert(struct TypeTable* hmap, struct Slice* key,
    enum TypeEntryType type, union TypeEntryVariant data){
  size_t label = hash_slice(key) % hmap->size;

  struct TypeEntry* new_entry = arena_alloc(sizeof(struct TypeEntry));
  new_entry->key = key;
  new_entry->type = type;
  new_entry->data = data;
  new_entry->next = NULL;

  if (hmap->arr[label] == NULL){
    hmap->arr[label] = new_entry;
  } else {
    struct TypeEntry* cur = hmap->arr[label];
    while (cur->next != NULL){
      cur = cur->next;
    }
    cur->next = new_entry;
  }
}

// Purpose: Look up a type entry by identifier name.
// Inputs: hmap is the table; key is the identifier slice.
// Outputs: Returns the entry or NULL if missing.
// Invariants/Assumptions: hash_slice is consistent with insertions.
struct TypeEntry* type_table_get(struct TypeTable* hmap, struct Slice* key){
  size_t label = hash_slice(key) % hmap->size;

  struct TypeEntry* cur = hmap->arr[label];
  while (cur != NULL){
    if (compare_slice_to_slice(cur->key, key)){
      return cur;
    }
    cur = cur->next;
  }
  return NULL;
}

// Purpose: Check if a type entry exists in the table.
// Inputs: hmap is the table; key is the identifier slice.
// Outputs: Returns true if the entry is present.
// Invariants/Assumptions: Performs a full lookup in the bucket chain.
bool type_table_contains(struct TypeTable* hmap, struct Slice* key){
  size_t label = hash_slice(key) % hmap->size;

  struct TypeEntry* cur = hmap->arr[label];
  while (cur != NULL){
    if (compare_slice_to_slice(cur->key, key)){
      return true;
    }
    cur = cur->next;
  }
  return false;
}

// Purpose: Print member entries for debugging.
// Inputs: members is the member chain to print.
// Outputs: Writes a human-readable dump to stdout.
// Invariants/Assumptions: Intended for debugging only.
void print_type_table(struct TypeTable* hmap){
  (void)hmap;
}

// Purpose: Print identifier attributes for debugging.
// Inputs: attrs is the attribute structure.
// Outputs: Writes a readable description to stdout.
// Invariants/Assumptions: Intended for debugging only.
void print_ident_attr(struct IdentAttr* attrs){
  (void)attrs;
}

// Purpose: Print initializer metadata for debugging.
// Inputs: init is the initializer structure.
// Outputs: Writes a readable description to stdout.
// Invariants/Assumptions: Intended for debugging only.
void print_ident_init(struct IdentInit* init){
  (void)init;
}

bool eval_const(struct Expr* expr, uint64_t* out_value) {
  if (expr == NULL || out_value == NULL) {
    return false;
  }
  switch (expr->type) {
    case LIT: {
      struct LitExpr* lit_expr = &expr->expr.lit_expr;
      switch (lit_expr->type) {
        case INT_CONST:
          *out_value = (uint64_t)lit_expr->value.int_val;
          return true;
        case UINT_CONST:
          *out_value = lit_expr->value.uint_val;
          return true;
        case LONG_CONST:
        case ULONG_CONST:
          return false;
        default:
          return false; // Not a constant literal
      }
    }
    case CAST: {
      struct CastExpr* cast_expr = &expr->expr.cast_expr;
      uint64_t inner_value;
      if (!eval_const(cast_expr->expr, &inner_value)) {
        return false;
      }
      // For simplicity, we assume casts do not change the value in this context.
      *out_value = inner_value;
      return true;
    }
    case BINARY: {
      struct BinaryExpr* bin_expr = &expr->expr.bin_expr;
      if (expr->value_type == NULL) {
        if (!typecheck_expr(expr)) {
          return false;
        }
      }
      bool is_signed = is_signed_type(expr->value_type);
      uint64_t left_value;
      uint64_t right_value;
      if (!eval_const(bin_expr->left, &left_value) ||
          !eval_const(bin_expr->right, &right_value)) {
        return false;
      }
      switch (bin_expr->op) {
        case ADD_OP:
          *out_value = left_value + right_value;
          return true;
        case SUB_OP:
          *out_value = left_value - right_value;
          return true;
        case MUL_OP:
          if (is_signed) {
            *out_value = (uint64_t)((int64_t)left_value * (int64_t)right_value);
          } else {
            *out_value = left_value * right_value;
          }
          return true;
        case DIV_OP:
          if (right_value == 0) {
            return false; // Division by zero
          }
          if (is_signed) {
            *out_value = (uint64_t)((int64_t)left_value / (int64_t)right_value);
          } else {
            *out_value = left_value / right_value;
          }
          return true;
        case MOD_OP:
          if (right_value == 0) {
            return false; // Modulo by zero
          }
          if (is_signed) {
            *out_value = (uint64_t)((int64_t)left_value % (int64_t)right_value);
          } else {
            *out_value = left_value % right_value;
          }
          return true;
        case BIT_AND:
          *out_value = left_value & right_value;
          return true;
        case BIT_OR:
          *out_value = left_value | right_value;
          return true;
        case BIT_XOR:
          *out_value = left_value ^ right_value;
          return true;
        case BIT_SHL:
          *out_value = left_value << right_value;
          return true;
        case BIT_SHR:
          if (is_signed) {
            *out_value = ((int64_t)left_value) >> right_value;
          } else {
            *out_value = left_value >> right_value;
          }
          return true;
        case BOOL_AND:
          *out_value = (left_value && right_value);
          return true;
        case BOOL_OR:
          *out_value = (left_value || right_value);
          return true;
        case BOOL_EQ:
          *out_value = (left_value == right_value);
          return true;
        case BOOL_NEQ:
          *out_value = (left_value != right_value);
          return true;
        case BOOL_LE:
          if (is_signed) {
            *out_value = ((int64_t)left_value < (int64_t)right_value);
          } else {
            *out_value = (left_value < right_value);
          }
          return true;
        case BOOL_GE:
          if (is_signed) {
            *out_value = ((int64_t)left_value > (int64_t)right_value);
          } else {
            *out_value = (left_value > right_value);
          }
          return true;
        case BOOL_LEQ:
          if (is_signed) {
            *out_value = ((int64_t)left_value <= (int64_t)right_value);
          } else {
            *out_value = (left_value <= right_value);
          }
          return true;
        case BOOL_GEQ:
          if (is_signed) {
            *out_value = ((int64_t)left_value >= (int64_t)right_value);
          } else {
            *out_value = (left_value >= right_value);
          }
          return true;
        case COMMA_OP:
          *out_value = right_value;
          return true;
        default:
          return false; // Unsupported binary operation for constant evaluation
      }
    }
    case UNARY: {
      struct UnaryExpr* unary_expr = &expr->expr.un_expr;
      if (expr->value_type == NULL) {
        if (!typecheck_expr(expr)) {
          return false;
        }
      }
      uint64_t inner_value;
      if (!eval_const(unary_expr->expr, &inner_value)) {
        return false;
      }
      switch (unary_expr->op) {
        case NEGATE:
          if (is_signed_type(expr->value_type)) {
            *out_value = (uint64_t)(-(int64_t)inner_value);
          } else {
            *out_value = (uint64_t)(~inner_value + 1); // Two's complement negation
          }
          return true;
        case COMPLEMENT:
          *out_value = ~inner_value;
          return true;
        case BOOL_NOT:
          *out_value = (inner_value == 0);
          return true;
        default:
          return false; // Unsupported unary operation for constant evaluation
      }
    }
    case SIZEOF_EXPR: {
      struct Expr* inner = expr->expr.sizeof_expr.expr;
      if (inner == NULL) {
        return false;
      }
      if (inner->value_type == NULL) {
        if (!typecheck_expr(inner)) {
          return false;
        }
      }
      if (!is_complete_type(inner->value_type)) {
        return false;
      }
      *out_value = (uint64_t)get_type_size(inner->value_type);
      return true;
    }
    case SIZEOF_T_EXPR: {
      struct Type* type = expr->expr.sizeof_t_expr.type;
      if (type == NULL) {
        return false;
      }
      if (!is_complete_type(type)) {
        return false;
      }
      *out_value = (uint64_t)get_type_size(type);
      return true;
    }
    case CONDITIONAL: {
      struct ConditionalExpr* cond_expr = &expr->expr.conditional_expr;
      uint64_t cond_value;
      if (!eval_const(cond_expr->condition, &cond_value)) {
        return false;
      }
      if (cond_value != 0) {
        return eval_const(cond_expr->left, out_value);
      } else {
        return eval_const(cond_expr->right, out_value);
      }
    }
    default:
      return false; // Not a literal expression
  }
  return false; // Not a literal expression
}

struct InitList* is_init_const(struct Type* type, struct Initializer* init) {
  switch (init->init_type){
    case SINGLE_INIT: {
      if (init->init.single_init->type == STRING) {
        struct StringExpr* str_expr = &init->init.single_init->expr.string_expr;

        struct InitList* init_list = arena_alloc(sizeof(struct InitList));
        init_list->next = NULL;
        init_list->value = arena_alloc(sizeof(struct StaticInit));
        init_list->value->int_type = STRING_INIT;
        init_list->value->value.string = str_expr->string;

        // type can be either char* or array of chars
        if (type->type == POINTER_TYPE && type->type_data.pointer_type.referenced_type->type == CHAR_TYPE) {
          // pointer to char

          struct Slice name_slice = {"string.label", 12};
          struct Slice* string_label = make_unique(&name_slice);

          struct Type* arr_type = arena_alloc(sizeof(struct Type));
          arr_type->type = ARRAY_TYPE;
          arr_type->type_data.array_type.size = str_expr->string->len + 1; // include null terminator
          arr_type->type_data.array_type.element_type = &kCharType;

          struct IdentAttr* const_attr = arena_alloc(sizeof(struct IdentAttr));
          const_attr->attr_type = CONST_ATTR;
          const_attr->is_defined = true;
          const_attr->storage = STATIC;
          const_attr->init.init_type = INITIAL;
          // Pad string literals with an explicit zero init to include the null terminator.
          size_t array_size = arr_type->type_data.array_type.size;
          size_t element_size = get_type_size(arr_type->type_data.array_type.element_type);
          if (str_expr->string->len < array_size) {
            struct InitList* pad_node = arena_alloc(sizeof(struct InitList));
            pad_node->value = arena_alloc(sizeof(struct StaticInit));
            pad_node->value->int_type = ZERO_INIT;
            pad_node->value->value.num = (array_size - str_expr->string->len) * element_size;
            pad_node->next = NULL;
            init_list->next = pad_node;
          }
          const_attr->init.init_list = init_list;

          // add string label to symbol table
          symbol_table_insert(global_symbol_table, string_label, arr_type, const_attr);

          // return pointer init to string label
          struct InitList* pointer_init = arena_alloc(sizeof(struct InitList));
          pointer_init->next = NULL;
          pointer_init->value = arena_alloc(sizeof(struct StaticInit));
          pointer_init->value->int_type = POINTER_INIT;
          pointer_init->value->value.pointer = string_label;

          return pointer_init;
        } else if (type->type == ARRAY_TYPE &&
                   is_char_type(type->type_data.array_type.element_type)) {
          // array of char-like elements

          // ensure array is large enough
          size_t array_size = type->type_data.array_type.size;
          if (str_expr->string->len > array_size) {
            type_error_at(init->init.single_init->loc, "string literal initializer too large for array");
            exit(1);
          }

          // append a zero_init node for padding
          size_t element_size = get_type_size(type->type_data.array_type.element_type);
          if (str_expr->string->len < array_size) {
            struct InitList* pad_node = arena_alloc(sizeof(struct InitList));
            pad_node->value = arena_alloc(sizeof(struct StaticInit));
            pad_node->value->int_type = ZERO_INIT;
            pad_node->value->value.num = (array_size - str_expr->string->len) * element_size;
            pad_node->next = NULL;
            init_list->next = pad_node;
          }

          return init_list;
        } else {
          type_error_at(init->init.single_init->loc, "string literal initializer type mismatch");
          exit(1);
        }
      } else {
        uint64_t const_val;
        if (!eval_const(init->init.single_init, &const_val)) {
          return NULL;
        }

        struct InitList* init_list = arena_alloc(sizeof(struct InitList));
        init_list->next = NULL;
        init_list->value = arena_alloc(sizeof(struct StaticInit));
        init_list->value->int_type = get_var_init(type);
        init_list->value->value.num = const_val;
        return init_list; 
      }
    }
    case COMPOUND_INIT: {
      if (type->type == ARRAY_TYPE) {
        // ensure each element is constant
        struct InitList* flattened = NULL;
        struct InitList* flattened_tail = NULL;
        size_t outer_count = 0;
        for (struct InitializerList* cur = init->init.compound_init; cur != NULL; cur = cur->next) {
          struct InitList* inner_init = is_init_const(type->type_data.array_type.element_type, cur->init);
          if (inner_init == NULL) {
            return NULL;
          }

          // append each element to flattened
          for (struct InitList* inner_cur = inner_init; inner_cur != NULL; inner_cur = inner_cur->next) {
            struct InitList* new_node = arena_alloc(sizeof(struct InitList));
            new_node->value = arena_alloc(sizeof(struct StaticInit));
            new_node->value->int_type = inner_cur->value->int_type;
            new_node->value->value = inner_cur->value->value;
            new_node->next = NULL;

            if (flattened == NULL) {
              flattened = new_node;
              flattened_tail = new_node;
            } else {
              flattened_tail->next = new_node;
              flattened_tail = new_node;
            }
          }
          outer_count++;
        }

        // pad to array length
        size_t array_len = type->type_data.array_type.size;
        size_t element_size = get_type_size(type->type_data.array_type.element_type);
        if (outer_count < array_len) {
          // append a zero_init node
          struct InitList* pad_node = arena_alloc(sizeof(struct InitList));
          pad_node->value = arena_alloc(sizeof(struct StaticInit));
          pad_node->value->int_type = ZERO_INIT;
          pad_node->value->value.num = (array_len - outer_count) * element_size;
          pad_node->next = NULL;
          if (flattened == NULL) {
            flattened = pad_node;
            flattened_tail = pad_node;
          } else {
            flattened_tail->next = pad_node;
            flattened_tail = pad_node;
          }
        }

        return flattened;
      } else if (type->type == STRUCT_TYPE) {
        if (init->init_type != COMPOUND_INIT) {
          type_error_at(NULL, "expected compound initializer for struct type");
          return NULL;
        }

        struct TypeEntry* struct_entry = type_table_get(global_type_table, type->type_data.struct_type.name);
        if (struct_entry == NULL) {
          type_error_at(NULL, "incomplete struct type in initializer");
          return NULL;
        }

        struct MemberEntry* member = struct_entry->data.struct_entry->members;
        struct InitializerList* init_cur = init->init.compound_init;

        struct InitList* init_list = NULL;
        struct InitList* init_list_tail = NULL;
        size_t offset = 0;
        while (member != NULL && init_cur != NULL) {
          struct InitList* member_init = is_init_const(member->type, init_cur->init);
          if (member_init == NULL) {
            return NULL;
          }

          // add padding if needed
          if (member->offset > offset) {
            struct InitList* pad_node = arena_alloc(sizeof(struct InitList));
            pad_node->value = arena_alloc(sizeof(struct StaticInit));
            pad_node->value->int_type = ZERO_INIT;
            pad_node->value->value.num = member->offset - offset;
            pad_node->next = NULL;

            if (init_list == NULL) {
              init_list = pad_node;
              init_list_tail = pad_node;
            } else {
              init_list_tail->next = pad_node;
              init_list_tail = pad_node;
            }
            offset = member->offset;
          }

          // append member_init to init_list
          struct InitList* more_inits = is_init_const(member->type, init_cur->init);
          if (more_inits == NULL) {
            return NULL;
          }
          if (init_list == NULL) {
            init_list = more_inits;
            init_list_tail = more_inits;
          } else {
            init_list_tail->next = more_inits;
          }
          while (init_list_tail->next != NULL) {
            init_list_tail = init_list_tail->next;
          }
          offset += get_type_size(member->type);

          member = member->next;
          init_cur = init_cur->next;
        }

        if (init_cur != NULL) {
          type_error_at(NULL, "too many initializers for struct type");
          return NULL;
        }

        // add final padding if needed
        size_t struct_size = get_type_size(type);
        if (offset < struct_size) {
          struct InitList* pad_node = arena_alloc(sizeof(struct InitList));
          pad_node->value = arena_alloc(sizeof(struct StaticInit));
          pad_node->value->int_type = ZERO_INIT;
          pad_node->value->value.num = struct_size - offset;
          pad_node->next = NULL;

          if (init_list == NULL) {
            init_list = pad_node;
            init_list_tail = pad_node;
          } else {
            init_list_tail->next = pad_node;
            init_list_tail = pad_node;
          }
        }

        return init_list;
      } else if (type->type == UNION_TYPE) {
        if (init->init_type != COMPOUND_INIT) {
          type_error_at(NULL, "expected compound initializer for union type");
          return NULL;
        }

        struct TypeEntry* union_entry = type_table_get(global_type_table, type->type_data.union_type.name);
        if (union_entry == NULL) {
          type_error_at(NULL, "incomplete union type in initializer");
          return NULL;
        }

        // only the first initializer is used for unions
        struct InitializerList* first_init = init->init.compound_init;
        if (first_init == NULL) {
          type_error_at(NULL, "empty initializer for union type");
          return NULL;
        }

        if (first_init->next != NULL) {
          type_error_at(NULL, "too many initializers for union type");
          return NULL;
        }

        struct InitList* union_init = is_init_const(
            union_entry->data.union_entry->members->type, first_init->init);
        if (union_init == NULL) {
          return NULL;
        }

        // pad to union size if needed
        size_t union_size = get_type_size(type);
        size_t init_size = get_type_size(
            union_entry->data.union_entry->members->type);
        if (init_size < union_size) {
          struct InitList* pad_node = arena_alloc(sizeof(struct InitList));
          pad_node->value = arena_alloc(sizeof(struct StaticInit));
          pad_node->value->int_type = ZERO_INIT;
          pad_node->value->value.num = union_size - init_size;
          pad_node->next = NULL;

          struct InitList* cur = union_init;
          while (cur->next != NULL) {
            cur = cur->next;
          }
          cur->next = pad_node;
        }

        return union_init;
      } else {
        // unsupported compound initializer type
        return NULL;
      }
    }
    default:
      return NULL;
  }
}

struct MemberEntry* get_struct_member(struct Type* type, struct Slice* member_name){
  if (type->type != STRUCT_TYPE && type->type != UNION_TYPE){
    return NULL;
  }

  struct TypeEntry* struct_entry = type_table_get(global_type_table, type->type_data.struct_type.name);
  if (struct_entry == NULL){
    return NULL;
  }

  for (struct MemberEntry* member = struct_entry->data.struct_entry->members;
       member != NULL; member = member->next){
    if (compare_slice_to_slice(member->key, member_name)){
      return member;
    }
  }
  return NULL;
}
