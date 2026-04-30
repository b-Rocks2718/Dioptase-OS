#include "identifier_resolution.h"
#include "identifier_map.h"
#include "unique_name.h"
#include "source_location.h"

#include "../crt/stdio.h"
#include "../crt/print.h"

// Purpose: Resolve identifiers to unique names and validate scoping rules.
// Inputs: Traverses AST nodes produced by the parser.
// Outputs: Rewrites identifier slices and reports resolution errors.
// Invariants/Assumptions: Uses a scoped identifier stack for name lookup.


// Inputs: Initialized in resolve_prog and updated on scope entry/exit.
static struct IdentStack* global_ident_stack = NULL; // function, variable, and enum constant namespace
static struct IdentStack* global_type_stack = NULL; // struct, union, and enum type namespace

// Purpose: Emit the shared identifier-resolution prefix for diagnostics.
// Inputs: loc points into source text and may be NULL.
// Outputs: Writes a best-effort location prefix to stderr.
// Invariants/Assumptions: source_location_from_ptr handles NULL/unknown locations.
static void ident_error_prefix(char* loc) {
  int args[3];
  struct SourceLocation where = source_location_from_ptr(loc);
  char* filename = source_filename_for_ptr(loc);

  if (where.line == 0) {
    fdputs(STDERR, "Identifier Resolution Error: ");
  } else {
    args[0] = (int)filename;
    args[1] = (int)where.line;
    args[2] = (int)where.column;
    fdprintf(STDERR, "Identifier Resolution Error at %s:%zu:%zu: ", args);
  }
}

// Purpose: Emit an identifier-resolution error with a fixed message.
// Inputs: loc points into source text and may be NULL; message is a static string.
// Outputs: Writes a diagnostic message to stderr.
// Invariants/Assumptions: Used for all bootstrap-bcc identifier-resolution failures
// that do not need formatted arguments.
static void ident_error_at(char* loc, char* message) {
  ident_error_prefix(loc);
  fdputs(STDERR, message);
  fdputs(STDERR, "\n");
}

// Purpose: Emit an identifier-resolution error that names one identifier slice.
// Inputs: loc points into source text and may be NULL; slice may be NULL.
// Outputs: Writes a diagnostic message to stderr.
// Invariants/Assumptions: fmt uses one %.*s placeholder pair.
static void ident_error_at1_slice(char* loc, char* fmt, struct Slice* slice) {
  int args[2];

  ident_error_prefix(loc);
  if (slice == NULL || slice->start == NULL) {
    args[0] = 6;
    args[1] = (int)"<null>";
  } else {
    args[0] = (int)slice->len;
    args[1] = (int)slice->start;
  }
  fdprintf(STDERR, fmt, args);
  fdputs(STDERR, "\n");
}

// Purpose: Locate the nearest identifier with linkage, ignoring local-only bindings.
// Inputs: stack is the identifier scope stack; name is the identifier to search.
// Outputs: Returns the first linkage-bearing entry found, or NULL if none exist.
// Invariants/Assumptions: Searches from innermost scope outward.
static struct IdentMapEntry* find_linkage_entry(struct IdentStack* stack,
                                                struct Slice* name) {
  for (int i = (int)stack->size - 1; i >= 0; --i) {
    struct IdentMapEntry* entry = ident_map_get(stack->maps[i], name);
    if (entry != NULL && entry->has_linkage) {
      return entry;
    }
  }
  return NULL;
}

// Purpose: Resolve identifiers for all file-scope declarations.
// Inputs: prog is the Program AST.
// Outputs: Returns true on success; false on any resolution error.
// Invariants/Assumptions: Initializes and destroys the global scope stack.
bool resolve_prog(struct Program* prog) {
  global_ident_stack = init_scope();
  global_type_stack = init_scope();

  for (struct DeclarationList* decl = prog->dclrs; decl != NULL; decl = decl->next) {
    if (!resolve_file_scope_dclr(&decl->dclr)) {
      ident_error_at(NULL, "failed to resolve declaration");
      return false;
    }
  }

  destroy_ident_stack(global_ident_stack);
  destroy_ident_stack(global_type_stack);

  return true;
}

// Purpose: Resolve identifiers within a function call argument list.
// Inputs: args is the argument list.
// Outputs: Returns true on success; false on any resolution error.
// Invariants/Assumptions: Uses the current scope stack.
bool resolve_args(struct ArgList* args){
  for (struct ArgList* arg = args; arg != NULL; arg = arg->next) {
    if (!resolve_expr(arg->arg)) {
      ident_error_at(arg->arg->loc, "failed to resolve argument");
      return false;
    }
  }
  return true;
}

bool resolve_var_init(struct Initializer* init){
  switch (init->init_type) {
    case SINGLE_INIT:
      return resolve_expr(init->init.single_init);
    case COMPOUND_INIT:
      for (struct InitializerList* item = init->init.compound_init; item != NULL; item = item->next) {
        if (!resolve_var_init(item->init)) {
          ident_error_at(NULL, "failed to resolve initializer list item");
          return false;
        }
      }
      return true;
    default:
      ident_error_at(NULL, "unknown initializer type");
      return false;
  }
}

// Purpose: Resolve identifiers within an expression subtree.
// Inputs: expr is the expression to resolve.
// Outputs: Returns true on success; false on any unresolved identifier.
// Invariants/Assumptions: Identifier stack is initialized before traversal.
bool resolve_expr(struct Expr* expr) {
  switch (expr->type) {
    case ASSIGN:
      if (!resolve_expr(expr->expr.assign_expr.left)) {
        return false;
      }
      return resolve_expr(expr->expr.assign_expr.right);
    case POST_ASSIGN:
      return resolve_expr(expr->expr.post_assign_expr.expr);
    case BINARY:
      if (!resolve_expr(expr->expr.bin_expr.left)) {
        return false;
      }
      return resolve_expr(expr->expr.bin_expr.right);
    case CONDITIONAL:
      if (!resolve_expr(expr->expr.conditional_expr.condition)) {
        return false;
      }
      if (!resolve_expr(expr->expr.conditional_expr.left)) {
        return false;
      }
      return resolve_expr(expr->expr.conditional_expr.right);
    case LIT:
      return true;
    case STRING:
      return true;
    case UNARY:
      return resolve_expr(expr->expr.un_expr.expr);
    case VAR: {
      struct IdentMapEntry* entry = ident_stack_get(global_ident_stack, expr->expr.var_expr.name, NULL);
      if (entry != NULL) {
        if (entry->is_const){
          // replace with literal expression
          expr->type = LIT;
          expr->expr.lit_expr.type = UINT_CONST;
          expr->expr.lit_expr.value.uint_val = entry->value;
        } else {
          // Substitute the unique name for locals so later passes can ignore scoping rules.
          expr->expr.var_expr.name = entry->entry_name;
        }
        return true;
      } else {
        struct Slice* var = expr->expr.var_expr.name;
        ident_error_at1_slice(expr->loc, "no declaration for variable %.*s", var);
        return false;
      }
    }
    case FUNCTION_CALL: {
      if (!resolve_expr(expr->expr.fun_call_expr.func)) {
        return false;
      }
      return resolve_args(expr->expr.fun_call_expr.args);
    }
    case CAST:
      return resolve_type(expr->expr.cast_expr.target) &&
             resolve_expr(expr->expr.cast_expr.expr);
    case ADDR_OF:
      return resolve_expr(expr->expr.addr_of_expr.expr);
    case DEREFERENCE:
      return resolve_expr(expr->expr.deref_expr.expr);
    case SUBSCRIPT:
      return resolve_expr(expr->expr.subscript_expr.array) &&
             resolve_expr(expr->expr.subscript_expr.index);
    case SIZEOF_EXPR:
      return resolve_expr(expr->expr.sizeof_expr.expr);
    case SIZEOF_T_EXPR:
      return resolve_type(expr->expr.sizeof_t_expr.type);
    case STMT_EXPR:
      return resolve_block(expr->expr.stmt_expr.block);
    case DOT_EXPR:
      return resolve_expr(expr->expr.dot_expr.struct_expr);
    case ARROW_EXPR:
      return resolve_expr(expr->expr.arrow_expr.pointer_expr);
    default:
      ident_error_at(expr->loc, "unknown expression type");
      return false;
  }
}

bool resolve_type(struct Type* type){
  switch (type->type){
    case STRUCT_TYPE:
    case UNION_TYPE:
    case ENUM_TYPE: {
      struct IdentMapEntry* entry = ident_stack_get(global_type_stack, type->type_data.struct_type.name, NULL);
      if (entry == NULL){
        ident_error_at(NULL, "specified an undeclared struct/union/enum type");
        return false;
      }

      if (entry->type != type->type) {
        ident_error_at(NULL, "type redeclared as different type");
        return false;
      }

      // rename to unique name
      type->type_data.struct_type.name = entry->entry_name;

      return true;
    }
    case POINTER_TYPE:
      return resolve_type(type->type_data.pointer_type.referenced_type);
    case ARRAY_TYPE:
      return resolve_type(type->type_data.array_type.element_type);
    case FUN_TYPE:
      // resolve parameter types in resolve_params
      return resolve_type(type->type_data.fun_type.return_type);
    default:
      return true;
  }
}

// Purpose: Resolve identifiers in a local variable declaration.
// Inputs: var_dclr is the variable declaration node.
// Outputs: Returns true on success; false on redeclaration or lookup errors.
// Invariants/Assumptions: Locals may be renamed to unique slices.
bool resolve_local_var_dclr(struct VariableDclr* var_dclr) {
  // extern and static declarations don't support cleanup attributes
  if (var_dclr->storage != NONE && var_dclr->attributes.cleanup_func != NULL) {
    ident_error_at(var_dclr->name->start, "extern/static variables cannot have cleanup attributes");
    return false;
  }

  if (!resolve_type(var_dclr->type)) {
    ident_error_at(var_dclr->name->start, "failed to resolve variable type");
    return false;
  }

  if (var_dclr->attributes.cleanup_func != NULL) {
    // ensure cleanup function is declared
    struct IdentMapEntry* entry = ident_stack_get(global_ident_stack, var_dclr->attributes.cleanup_func, NULL);
    if (entry == NULL) {
      ident_error_at(var_dclr->attributes.cleanup_func->start, "cleanup function not declared");
      return false;
    }
  }

  bool from_current_scope = false;
  struct IdentMapEntry* entry = ident_stack_get(global_ident_stack, var_dclr->name, &from_current_scope);
  if (var_dclr->storage == EXTERN) {
    if (entry != NULL && from_current_scope) {
      if (entry->has_linkage) {
        // redeclaration with extern linkage in the same block
        return true;
      }
      ident_error_at(var_dclr->name->start, "multiple declarations for variable");
      return false;
    }

    // Bind this block-scope extern to the nearest linkage-bearing declaration.
    struct IdentMapEntry* linkage_entry = find_linkage_entry(global_ident_stack, var_dclr->name);
    struct Slice* target_name = (linkage_entry != NULL) ? linkage_entry->entry_name : var_dclr->name;
    ident_stack_insert(global_ident_stack, var_dclr->name, target_name, true, -1, false, 0);
    return true;
  }

  if (entry != NULL) {
    if (from_current_scope) {
      // already declared in this scope
      ident_error_at(var_dclr->name->start, "multiple declarations for variable");
      return false;
    } else {
      // Declared in an outer scope; create a new unique local.
      struct Slice* unique_name = make_unique(var_dclr->name);
      ident_stack_insert(global_ident_stack, var_dclr->name,
          unique_name, false, -1, false, 0);
      var_dclr->name = unique_name;
      if (var_dclr->init != NULL) {
        return resolve_var_init(var_dclr->init);
      }
      return true;
    }
  }

  // First declaration in this scope: insert and optionally resolve initializer.
  struct Slice* unique_name = make_unique(var_dclr->name);
  ident_stack_insert(global_ident_stack, var_dclr->name,
      unique_name, false, -1, false, 0);
  var_dclr->name = unique_name;
  if (var_dclr->init != NULL) {
    return resolve_var_init(var_dclr->init);
  }
  return true;
}

// Purpose: Resolve identifiers in a local declaration (var or func).
// Inputs: dclr is the declaration node.
// Outputs: Returns true on success; false on any resolution error.
// Invariants/Assumptions: Caller manages scope entry/exit.
bool resolve_local_dclr(struct Declaration* dclr) {
  switch (dclr->type) {
    case VAR_DCLR:
      return resolve_local_var_dclr(&dclr->dclr.var_dclr);
    case FUN_DCLR:
      return resolve_local_func(&dclr->dclr.fun_dclr);
    case STRUCT_DCLR:
      return resolve_struct(&dclr->dclr.struct_dclr);
    case UNION_DCLR:
      return resolve_union(&dclr->dclr.union_dclr);
    case ENUM_DCLR:
      return resolve_enum(&dclr->dclr.enum_dclr);
    default:
      ident_error_at(NULL, "unknown declaration type");
      return false;
  }
}

// Purpose: Resolve identifiers in a for-loop initializer.
// Inputs: init is the initializer node.
// Outputs: Returns true on success; false on any resolution error.
// Invariants/Assumptions: A for-loop introduces its own scope.
bool resolve_for_init(struct ForInit* init) {
  switch (init->type) {
    case DCLR_INIT:
      return resolve_local_var_dclr(init->init.dclr_init);
    case EXPR_INIT:
      if (init->init.expr_init != NULL) {
        return resolve_expr(init->init.expr_init);
      } else {
        return true;
      }
    default:
      ident_error_at(NULL, "unknown for init type");
      return false;
  }
}

// Purpose: Resolve identifiers within a statement subtree.
// Inputs: stmt is the statement node.
// Outputs: Returns true on success; false on any resolution error.
// Invariants/Assumptions: Manages scope for compound and for statements.
bool resolve_stmt(struct Statement* stmt) {
  switch (stmt->type) {
    case RETURN_STMT:
      if (!stmt->statement.ret_stmt.expr) {
        // void return, nothing to resolve
        return true;
      }
      return resolve_expr(stmt->statement.ret_stmt.expr);
    case EXPR_STMT:
      return resolve_expr(stmt->statement.expr_stmt.expr);
    case IF_STMT:
      if (!resolve_expr(stmt->statement.if_stmt.condition)) {
        return false;
      }
      if (!resolve_stmt(stmt->statement.if_stmt.if_stmt)) {
        return false;
      }
      if (stmt->statement.if_stmt.else_stmt != NULL) {
        if (!resolve_stmt(stmt->statement.if_stmt.else_stmt)) {
          return false;
        }
      }
      return true;
    case LABELED_STMT:
      return resolve_stmt(stmt->statement.labeled_stmt.stmt);
    case GOTO_STMT:
      return true;
    case COMPOUND_STMT:
      // New scope for block-local declarations.
      enter_scope(global_ident_stack);
      enter_scope(global_type_stack);
      if (!resolve_block(stmt->statement.compound_stmt.block)) {
        return false;
      }
      struct IdentMap* maps = exit_scope(global_ident_stack);
      exit_scope(global_type_stack);
      if (stmt->statement.compound_stmt.block != NULL) {
        stmt->statement.compound_stmt.block->idents = maps;
      } else {
        destroy_ident_map(maps);
      }
      return true;
    case BREAK_STMT:
      return true;
    case CONTINUE_STMT:
      return true;
    case WHILE_STMT:
      if (!resolve_expr(stmt->statement.while_stmt.condition)) {
        return false;
      }
      return resolve_stmt(stmt->statement.while_stmt.statement);
    case DO_WHILE_STMT:
      if (!resolve_stmt(stmt->statement.do_while_stmt.statement)) {
        return false;
      }
      return resolve_expr(stmt->statement.do_while_stmt.condition);
    case FOR_STMT:
      // The for-init may introduce new locals, so give the loop its own scope.
      enter_scope(global_ident_stack);
      enter_scope(global_type_stack);
      if (!resolve_for_init(stmt->statement.for_stmt.init)) {
        return false;
      }

      if (stmt->statement.for_stmt.condition != NULL && 
          !resolve_expr(stmt->statement.for_stmt.condition)) {
        return false;
      }

      if (stmt->statement.for_stmt.end != NULL && 
          !resolve_expr(stmt->statement.for_stmt.end)) {
        return false;
      }

      if (!resolve_stmt(stmt->statement.for_stmt.statement)) {
        return false;
      }
      stmt->statement.for_stmt.init_idents = exit_scope(global_ident_stack);
      exit_scope(global_type_stack);
      return true;
    case SWITCH_STMT:
      if (!resolve_expr(stmt->statement.switch_stmt.condition)) {
        return false;
      }
      return resolve_stmt(stmt->statement.switch_stmt.statement);
    case CASE_STMT:
      if (!resolve_expr(stmt->statement.case_stmt.expr)) {
        return false;
      }
      return resolve_stmt(stmt->statement.case_stmt.statement);
    case DEFAULT_STMT:
      return resolve_stmt(stmt->statement.default_stmt.statement);
    case NULL_STMT:
      return true;
    default:
      ident_error_at(stmt->loc, "unknown statement type");
      return false;
  }
}

// Purpose: Resolve identifiers for a local function declaration.
// Inputs: func_dclr is the function declaration node.
// Outputs: Returns true on success; false on invalid linkage or body use.
// Invariants/Assumptions: Local function declarations must be extern-only.
bool resolve_local_func(struct FunctionDclr* func_dclr) {
  // local functions must have extern linkage
  if (func_dclr->storage == STATIC) {
      ident_error_at(func_dclr->name->start,
                     "local function declarations cannot be static");
      return false;
  }

  // local functions cannot have bodies
  if (func_dclr->body != NULL) {
    ident_error_at(func_dclr->name->start, "local function cannot have body");
    return false;
  }

  bool from_current_scope = false;
  struct IdentMapEntry* entry = ident_stack_get(global_ident_stack, func_dclr->name, &from_current_scope);
  if (entry != NULL && from_current_scope && !entry->has_linkage) {
    ident_error_at(func_dclr->name->start,
                   "function declaration conflicts with existing local identifier");
    return false;
  }

  if (entry == NULL || !from_current_scope) {
    // Insert into the current scope so inner blocks can shadow outer identifiers.
    ident_stack_insert(global_ident_stack, func_dclr->name,
        func_dclr->name, true, -1, false, 0);
  }

  return true;
}

// Purpose: Resolve identifiers for each parameter in a parameter list.
// Inputs: params is the parameter list.
// Outputs: Returns true on success; false on any resolution error.
// Invariants/Assumptions: Parameters are resolved as local variables.
bool resolve_params(struct ParamList* params){
  for (struct ParamList* param = params; param != NULL; param = param->next) {
    if (param->param.name == NULL) {
      ident_error_at(NULL, "unnamed function parameter not supported");
      return false;
    }
    if (!resolve_local_var_dclr(&param->param)) {
      ident_error_at(param->param.name->start, "failed to resolve parameter");
      return false;
    }
  }
  return true;
}

// Purpose: Resolve identifiers within a block list.
// Inputs: block is the block list.
// Outputs: Returns true on success; false on any resolution error.
// Invariants/Assumptions: Does not automatically enter/exit scope.
bool resolve_block(struct Block* block){
  for (struct Block* item = block; item != NULL; item = item->next) {
    switch (item->item->type) {
      case STMT_ITEM:
        if (!resolve_stmt(item->item->item.stmt)) {
          ident_error_at(item->item->item.stmt->loc, "failed to resolve statement in block");
          return false;
        }
        break;
      case DCLR_ITEM:
        if (!resolve_local_dclr(item->item->item.dclr)) {
          ident_error_at(NULL, "failed to resolve declaration in block");
          return false;
        }
        break;
      default:
        ident_error_at(NULL, "unknown block item type");
        return false;
    }
  }
  return true;
}

// Purpose: Resolve identifiers in a file-scope variable declaration.
// Inputs: var_dclr is the variable declaration node.
// Outputs: Returns true on success; false on invalid redeclarations.
// Invariants/Assumptions: File-scope variables keep their original names.
bool resolve_file_scope_var_dclr(struct VariableDclr* var_dclr) {
  // file scope vars don't support cleanup attributes
  if (var_dclr->attributes.cleanup_func != NULL) {
    ident_error_at(var_dclr->name->start, "file-scope variables cannot have cleanup attributes");
    return false;
  }

  if (!resolve_type(var_dclr->type)) {
    ident_error_at(var_dclr->name->start, "failed to resolve variable type");
    return false;
  }

  if (var_dclr->init != NULL && !resolve_var_init(var_dclr->init)) {
    ident_error_at(var_dclr->name->start, "failed to resolve variable initializer");
    return false;
  }

  bool from_current_scope = false;
  struct IdentMapEntry* entry = ident_stack_get(global_ident_stack, var_dclr->name, &from_current_scope);
  if (entry != NULL) {
    if (!from_current_scope) {
      // this should never happen, as file scope declarations are global
      ident_error_at(var_dclr->name->start, "declaration is outside file scope");
      return false;
    }
    
    // already declared
    return true;
  } else {
    // add to ident map
    ident_stack_insert(global_ident_stack, var_dclr->name,
        var_dclr->name, var_dclr->storage != STATIC, -1, false, 0);
    // expr should be a constant if it exists
    // no need to recursively process
    return true;
  }
}

// Purpose: Resolve identifiers in a file-scope function declaration/definition.
// Inputs: func_dclr is the function declaration node.
// Outputs: Returns true on success; false on invalid redeclarations.
// Invariants/Assumptions: Function bodies get their own scope for params/locals.
bool resolve_file_scope_func(struct FunctionDclr* func_dclr) {
  if (!resolve_type(func_dclr->type)) {
    ident_error_at(func_dclr->name->start, "failed to resolve function return type");
    return false;
  }

  bool from_current_scope = false;
  struct IdentMapEntry* entry = ident_stack_get(global_ident_stack, func_dclr->name, &from_current_scope);
  if (entry != NULL) {
    if (!from_current_scope) {
      // this should never happen, as file scope declarations are global
      ident_error_at(func_dclr->name->start, "declaration is outside file scope");
      return false;
    }

    // Internal linkage functions can be declared multiple times; reuse the entry.

    if (func_dclr->body == NULL) {
      // just a declaration
      return true;
    }

    // definition after a prior declaration; resolve params/body in a new scope
    enter_scope(global_ident_stack);
    enter_scope(global_type_stack);
    bool params_resolved = resolve_params(func_dclr->params);
    bool block_resolved = resolve_block(func_dclr->body);
    func_dclr->body->idents = exit_scope(global_ident_stack);
    exit_scope(global_type_stack);
    return params_resolved && block_resolved;
  } else {
    // add to ident map
    ident_stack_insert(global_ident_stack, func_dclr->name,
        func_dclr->name, func_dclr->storage != STATIC, -1, false, 0);

    // Parameters and body share a new scope distinct from file scope.
    enter_scope(global_ident_stack);
    enter_scope(global_type_stack);
    bool params_resolved = resolve_params(func_dclr->params);
    bool block_resolved = resolve_block(func_dclr->body);
    
    struct IdentMap* maps = exit_scope(global_ident_stack);
    exit_scope(global_type_stack);
    if (func_dclr->body != NULL){
      func_dclr->body->idents = maps;
    } else {
      destroy_ident_map(maps);
    }

    return params_resolved && block_resolved;
  }
}

bool resolve_struct(struct StructDclr* struct_dclr){
  bool from_current_scope = false;
  struct IdentMapEntry* entry = ident_stack_get(global_type_stack, struct_dclr->name, &from_current_scope);
  if (entry == NULL || !from_current_scope){
    // new type declaration
    struct Slice* unique_name = make_unique(struct_dclr->name);
    ident_stack_insert(global_type_stack, struct_dclr->name, unique_name, false, STRUCT_TYPE, false, 0);
    struct_dclr->name = unique_name;
  } else {
    // redeclaration of existing type
    if (entry->type != STRUCT_TYPE) {
      ident_error_at(struct_dclr->name->start, "type redeclared as different type");
      return false;
    }

    // switch to unique name
    struct_dclr->name = entry->entry_name;
  }

  // resolve member types
  for (struct MemberDclr* member = struct_dclr->members; member != NULL; member = member->next){
    if (!resolve_type(member->type)){
      ident_error_at(member->name->start, "failed to resolve struct member type");
      return false;
    }
  }

  return true;
}

bool resolve_union(struct UnionDclr* union_dclr){
  bool from_current_scope = false;
  struct IdentMapEntry* entry = ident_stack_get(global_type_stack, union_dclr->name, &from_current_scope);
  if (entry == NULL || !from_current_scope){
    // new type declaration
    struct Slice* unique_name = make_unique(union_dclr->name);
    ident_stack_insert(global_type_stack, union_dclr->name, unique_name, false, UNION_TYPE, false, 0);
    union_dclr->name = unique_name;
  } else {
    // redeclaration of existing type
    if (entry->type != UNION_TYPE) {
      ident_error_at(union_dclr->name->start, "type redeclared as different type");
      return false;
    }

    // switch to unique name
    union_dclr->name = entry->entry_name;
  }

  // resolve member types
  for (struct MemberDclr* member = union_dclr->members; member != NULL; member = member->next){
    if (!resolve_type(member->type)){
      ident_error_at(member->name->start, "failed to resolve union member type");
      return false;
    }
  }

  return true;
}

bool resolve_enum(struct EnumDclr* enum_dclr){
  bool from_current_scope = false;
  struct IdentMapEntry* entry = ident_stack_get(global_type_stack, enum_dclr->name, &from_current_scope);
  if (entry == NULL || !from_current_scope){
    // new type declaration
    struct Slice* unique_name = make_unique(enum_dclr->name);
    ident_stack_insert(global_type_stack, enum_dclr->name, unique_name, false, ENUM_TYPE, false, 0);
    enum_dclr->name = unique_name;
  } else {
    // redeclaration of existing type
    if (entry->type != ENUM_TYPE) {
      ident_error_at(enum_dclr->name->start, "type redeclared as different type");
      return false;
    }

    // switch to unique name
    enum_dclr->name = entry->entry_name;
  }

  // add enum member values to ident map
  for (struct EnumMemberDclr* member = enum_dclr->members; member != NULL; member = member->next){
    // check if member already exists
    bool from_current_scope = false;
    struct IdentMapEntry* member_entry = ident_stack_get(global_ident_stack, member->name, &from_current_scope);
    if (member_entry == NULL || !from_current_scope){
      // new enum member
      struct Slice* unique_member_name = make_unique(member->name);
      ident_stack_insert(global_ident_stack, member->name, unique_member_name, false, -1, true, member->value);
      member->name = unique_member_name;
    } else {
      ident_error_at(member->name->start, "redeclaration of enum member");
      return false;
    }
  }

  return true;
}

// Purpose: Resolve identifiers in a file-scope declaration.
// Inputs: dclr is the declaration node.
// Outputs: Returns true on success; false on any resolution error.
// Invariants/Assumptions: File-scope declarations share one global scope.
bool resolve_file_scope_dclr(struct Declaration* dclr) {
  switch (dclr->type) {
    case VAR_DCLR:
      return resolve_file_scope_var_dclr(&dclr->dclr.var_dclr);
    case FUN_DCLR:
      return resolve_file_scope_func(&dclr->dclr.fun_dclr);
    case STRUCT_DCLR:
      return resolve_struct(&dclr->dclr.struct_dclr);
    case UNION_DCLR:
      return resolve_union(&dclr->dclr.union_dclr);
    case ENUM_DCLR:
      return resolve_enum(&dclr->dclr.enum_dclr);
    default:
      ident_error_at(NULL, "Unknown declaration type");
      return false;
  }
}
