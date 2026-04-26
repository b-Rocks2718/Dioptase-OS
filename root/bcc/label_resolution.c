#include "label_resolution.h"
#include "unique_name.h"
#include "label_map.h"
#include "arena.h"
#include "source_location.h"

#include "../crt/stdio.h"
#include "../crt/print.h"
#include "../crt/stdint.h"

// Purpose: Resolve control-flow labels (loops/switches/gotos/cases) in the AST.
// Inputs: Traverses Program, Block, and Statement nodes produced by parsing.
// Outputs: Annotates statements with unique labels and case lists.
// Invariants/Assumptions: Labels are slices allocated from the arena.

// Purpose: Track the innermost loop/switch labels for break/continue/case.
// Inputs: Updated on entry/exit of loop or switch statements.
// Outputs: Referenced when labeling break/continue/case/default nodes.
// Invariants/Assumptions: Only one active loop and one active switch are tracked.
struct Slice* cur_loop_label = NULL;
struct Slice* cur_switch_label = NULL;
enum LabelType cur_label_type = -1;

// Purpose: Map user goto labels to unique labels within a function.
// Inputs: Populated during label_stmt traversal of labeled statements.
// Outputs: Used by resolve_gotos to rewrite goto targets.
// Invariants/Assumptions: Recreated per function and destroyed after labeling.
struct LabelMap* goto_labels = NULL;
// Purpose: Collects case/default labels for the current switch statement.
// Inputs: Appended during collect_cases traversal.
// Outputs: Stored on the switch statement node.
// Invariants/Assumptions: Reset when entering/leaving a switch statement.
struct CaseList* current_case_list = NULL;

// Purpose: Emit the shared label-resolution prefix at a source location.
// Inputs: prefix identifies the pass; loc points into source text and may be NULL.
// Outputs: Writes a best-effort message prefix to stderr.
// Invariants/Assumptions: source_location_from_ptr handles NULL/unknown locations.
static void label_error_prefix(char* prefix, char* loc) {
  int args[3];
  struct SourceLocation where = source_location_from_ptr(loc);
  char* filename = source_filename_for_ptr(loc);

  if (where.line == 0) {
    fdputs(STDERR, prefix);
    fdputs(STDERR, ": ");
  } else {
    fdputs(STDERR, prefix);
    fdputs(STDERR, " at ");
    args[0] = (int)filename;
    args[1] = (int)where.line;
    args[2] = (int)where.column;
    fdprintf(STDERR, "%s:%zu:%zu: ", args);
  }
}

// Purpose: Emit a label-resolution error with a fixed message.
// Inputs: prefix identifies the pass; loc points into source text and may be NULL.
// Outputs: Writes a diagnostic message to stderr.
// Invariants/Assumptions: Used when no formatted arguments are needed.
static void label_error_at(char* prefix, char* loc, char* message) {
  label_error_prefix(prefix, loc);
  fdputs(STDERR, message);
  fdputs(STDERR, "\n");
}

// Purpose: Emit a label-resolution error that names one slice.
// Inputs: prefix identifies the pass; loc points into source text; slice may be NULL.
// Outputs: Writes a diagnostic message to stderr.
// Invariants/Assumptions: fmt uses one %.*s placeholder pair.
static void label_error_at1_slice(char* prefix, char* loc, char* fmt, struct Slice* slice) {
  int args[2];

  label_error_prefix(prefix, loc);
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

// Purpose: Emit a label-resolution error that names one integer value.
// Inputs: prefix identifies the pass; loc points into source text and may be NULL.
// Outputs: Writes a diagnostic message to stderr.
// Invariants/Assumptions: fmt uses one %d placeholder.
static void label_error_at1_int(char* prefix, char* loc, char* fmt, int value) {
  int args[1];

  args[0] = value;
  label_error_prefix(prefix, loc);
  fdprintf(STDERR, fmt, args);
  fdputs(STDERR, "\n");
}

// Purpose: Compute the decimal digit length of a 32-bit unsigned value.
// Inputs: value is the unsigned integer to measure.
// Outputs: Returns the number of base-10 digits needed.
// Invariants/Assumptions: Always returns at least 1.
static unsigned u32_len(uint32_t value) {
  unsigned len = 0;
  do {
    len++;
    value /= 10u;
  } while (value != 0u);
  return len;
}

// Purpose: Label all functions in a program and resolve gotos/cases.
// Inputs: prog is the AST for the full translation unit.
// Outputs: Returns true on success; false on any labeling error.
// Invariants/Assumptions: Each function body is labeled independently.
bool label_loops(struct Program* prog) {
  for (struct DeclarationList* decl = prog->dclrs; decl != NULL; decl = decl->next) {
    // only need to label functions, not global variables
    if (decl->dclr.type == FUN_DCLR) {
      struct FunctionDclr* func_dclr = &decl->dclr.dclr.fun_dclr;
      // only label if there is a body
      if (func_dclr->body != NULL) {
        // Each function gets its own goto-label map and labeling pass.
        goto_labels = create_label_map(256);

        if (!label_block(func_dclr->name, func_dclr->body)) {
          return false;
        }

        if (!resolve_gotos(func_dclr->body)) {
          return false;
        }

        if (!collect_cases(func_dclr->body)) {
          return false;
        }

        destroy_label_map(goto_labels);
      }
    }
  }

  return true;
}

// -------------------------------- loop/switch labeling -------------------------------- //

bool label_expr(struct Slice* func_name, struct Expr* expr){
  switch (expr->type) {
    case BINARY: {
      struct BinaryExpr* bin_expr = &expr->expr.bin_expr;
      if (!label_expr(func_name, bin_expr->left)) {
        return false;
      }
      if (!label_expr(func_name, bin_expr->right)) {
        return false;
      }
      break;
    }
    case ASSIGN: {
      struct AssignExpr* assign_expr = &expr->expr.assign_expr;
      if (!label_expr(func_name, assign_expr->left)) {
        return false;
      }
      if (!label_expr(func_name, assign_expr->right)) {
        return false;
      }
      break;
    }
    case POST_ASSIGN: {
      struct PostAssignExpr* post_assign_expr = &expr->expr.post_assign_expr;
      if (!label_expr(func_name, post_assign_expr->expr)) {
        return false;
      }
      break;
    }
    case CONDITIONAL: {
      struct ConditionalExpr* cond_expr = &expr->expr.conditional_expr;
      if (!label_expr(func_name, cond_expr->condition)) {
        return false;
      }
      if (!label_expr(func_name, cond_expr->left)) {
        return false;
      }
      if (!label_expr(func_name, cond_expr->right)) {
        return false;
      }
      break;
    }
    case CAST: {
      struct CastExpr* cast_expr = &expr->expr.cast_expr;
      if (!label_expr(func_name, cast_expr->expr)) {
        return false;
      }
      break;
    }
    case FUNCTION_CALL: {
      struct FunctionCallExpr* fun_call_expr = &expr->expr.fun_call_expr;
      if (!label_expr(func_name, fun_call_expr->func)) {
        return false;
      }
      for (struct ArgList* arg = fun_call_expr->args; arg != NULL; arg = arg->next) {
        if (!label_expr(func_name, arg->arg)) {
          return false;
        }
      }
      break;
    }
    case ADDR_OF: {
      struct AddrOfExpr* addr_of_expr = &expr->expr.addr_of_expr;
      if (!label_expr(func_name, addr_of_expr->expr)) {
        return false;
      }
      break;
    }
    case DEREFERENCE: {
      struct DereferenceExpr* deref_expr = &expr->expr.deref_expr;
      if (!label_expr(func_name, deref_expr->expr)) {
        return false;
      }
      break;
    }
    case SUBSCRIPT: {
      struct SubscriptExpr* subscript_expr = &expr->expr.subscript_expr;
      if (!label_expr(func_name, subscript_expr->array)) {
        return false;
      }
      if (!label_expr(func_name, subscript_expr->index)) {
        return false;
      }
      break;
    }
    case SIZEOF_EXPR: {
      struct SizeOfExpr* sizeof_expr = &expr->expr.sizeof_expr;
      if (!label_expr(func_name, sizeof_expr->expr)) {
        return false;
      }
      break;
    }
    case STMT_EXPR: {
      struct StmtExpr* stmt_expr = &expr->expr.stmt_expr;
      if (!label_block(func_name, stmt_expr->block)) {
        return false;
      }
      break;
    }
    case DOT_EXPR: {
      struct DotExpr* dot_expr = &expr->expr.dot_expr;
      if (!label_expr(func_name, dot_expr->struct_expr)){
        return false;
      }
      break;
    }
    case ARROW_EXPR: {
      struct ArrowExpr* arrow_expr = &expr->expr.arrow_expr;
      if (!label_expr(func_name, arrow_expr->pointer_expr)){
        return false;
      }
      break;
    }
    default:
      // No labels needed in other expression types
      break;
  }
  return true;
}

// Purpose: Label any statement expressions embedded in an initializer tree.
// Inputs: func_name is the enclosing function name; init is the initializer node.
// Outputs: Returns true on success; false on any labeling error.
// Invariants/Assumptions: Compound initializers may nest arbitrarily.
static bool label_initializer(struct Slice* func_name, struct Initializer* init) {
  if (init == NULL) {
    return true;
  }

  switch (init->init_type) {
    case SINGLE_INIT:
      if (init->init.single_init != NULL) {
        return label_expr(func_name, init->init.single_init);
      }
      return true;
    case COMPOUND_INIT:
      for (struct InitializerList* item = init->init.compound_init; item != NULL; item = item->next) {
        if (!label_initializer(func_name, item->init)) {
          return false;
        }
      }
      return true;
    default:
      label_error_at("Loop Labeling Error", init->loc,
                     "unknown initializer type");
      return false;
  }
}

// Purpose: Label statement expressions referenced by a local declaration.
// Inputs: func_name is the enclosing function name; dclr is the declaration node.
// Outputs: Returns true on success; false on any labeling error.
// Invariants/Assumptions: Local function declarations do not have bodies.
static bool label_local_dclr(struct Slice* func_name, struct Declaration* dclr) {
  switch (dclr->type) {
    case VAR_DCLR:
      return label_initializer(func_name, dclr->dclr.var_dclr.init);
    case FUN_DCLR:
      return true;
    case STRUCT_DCLR:
    case UNION_DCLR:
    case ENUM_DCLR:
      return true;
    default:
      label_error_at("Loop Labeling Error", NULL,
                     "unknown declaration type");
      return false;
  }
}

// Purpose: Label statement expressions used in a for-loop initializer.
// Inputs: func_name is the enclosing function name; init is the for initializer.
// Outputs: Returns true on success; false on any labeling error.
// Invariants/Assumptions: The initializer is either a declaration or expression.
static bool label_for_init(struct Slice* func_name, struct ForInit* init) {
  if (init == NULL) {
    return true;
  }

  switch (init->type) {
    case DCLR_INIT:
      return label_initializer(func_name, init->init.dclr_init->init);
    case EXPR_INIT:
      if (init->init.expr_init != NULL) {
        return label_expr(func_name, init->init.expr_init);
      }
      return true;
    default:
      label_error_at("Loop Labeling Error", NULL,
                     "unknown for initializer type");
      return false;
  }
}

// Purpose: Apply label assignment to a statement subtree.
// Inputs: func_name is the enclosing function name; stmt is the node to label.
// Outputs: Returns true on success; false on any invalid label usage.
// Invariants/Assumptions: Uses global label state to handle nesting.
bool label_stmt(struct Slice* func_name, struct Statement* stmt) {
  switch (stmt->type) {
    case WHILE_STMT: {
      if (!label_expr(func_name, stmt->statement.while_stmt.condition)) {
        return false;
      }
      struct WhileStmt* while_stmt = &stmt->statement.while_stmt;
      // generate label
      struct Slice* label = make_unique_label(func_name, "while");

      // save previous label state
      enum LabelType prev_label_type = cur_label_type;
      struct Slice* prev_loop_label = cur_loop_label;

      // Entered a loop: update current label state for break/continue.
      cur_loop_label = label;
      cur_label_type = LOOP;

      // label body
      if (!label_stmt(func_name, while_stmt->statement)) {
        return false;
      }

      // assign label
      while_stmt->label = label;

      // restore previous label state
      cur_loop_label = prev_loop_label;
      cur_label_type = prev_label_type;

      break;
    }
    case DO_WHILE_STMT: {
      struct DoWhileStmt* do_while_stmt = &stmt->statement.do_while_stmt;
      // generate label
      struct Slice* label = make_unique_label(func_name, "do_while");

      // save previous label state
      enum LabelType prev_label_type = cur_label_type;
      struct Slice* prev_loop_label = cur_loop_label;

      // Entered a loop: update current label state for break/continue.
      cur_loop_label = label;
      cur_label_type = LOOP;

      // label body
      if (!label_stmt(func_name, do_while_stmt->statement)) {
        return false;
      }

      if (!label_expr(func_name, do_while_stmt->condition)) {
        return false;
      }

      // assign label
      do_while_stmt->label = label;

      // restore previous label state
      cur_loop_label = prev_loop_label;
      cur_label_type = prev_label_type;

      break;
    }
    case FOR_STMT: {
      struct ForStmt* for_stmt = &stmt->statement.for_stmt;
      // generate label
      struct Slice* label = make_unique_label(func_name, "for");

      // save previous label state
      enum LabelType prev_label_type = cur_label_type;
      struct Slice* prev_loop_label = cur_loop_label;

      // Entered a loop: update current label state for break/continue.
      cur_loop_label = label;
      cur_label_type = LOOP;

      // label body
      if (!label_stmt(func_name, for_stmt->statement)) {
        return false;
      }
      if (!label_for_init(func_name, for_stmt->init)) {
        return false;
      }
      if (for_stmt->condition != NULL && !label_expr(func_name, for_stmt->condition)) {
        return false;
      }
      if (for_stmt->end != NULL && !label_expr(func_name, for_stmt->end)) {
        return false;
      }
      // assign label
      for_stmt->label = label;

      // restore previous label state
      cur_loop_label = prev_loop_label;
      cur_label_type = prev_label_type;

      break;
    }
    case SWITCH_STMT: {
      struct SwitchStmt* switch_stmt = &stmt->statement.switch_stmt;
      // generate label
      struct Slice* label = make_unique_label(func_name, "switch");

      // save previous label state
      enum LabelType prev_label_type = cur_label_type;
      struct Slice* prev_switch_label = cur_switch_label;

      // Entered a switch: update current label state for break/case/default.
      cur_switch_label = label;
      cur_label_type = SWITCH;

      if (!label_expr(func_name, switch_stmt->condition)) {
        return false;
      }

      // label body
      if (!label_stmt(func_name, switch_stmt->statement)) {
        return false;
      }
      // assign label
      switch_stmt->label = label;

      // restore previous label state
      cur_switch_label = prev_switch_label;
      cur_label_type = prev_label_type;

      break;
    }
    case BREAK_STMT: {
      struct BreakStmt* break_stmt = &stmt->statement.break_stmt;

      if (cur_label_type == -1) {
        label_error_at("Loop Labeling Error", stmt->loc,
                       "break statement outside loop/switch");
        return false;
      }

      // assign current label to break statement
      break_stmt->label = cur_label_type == LOOP ? cur_loop_label : cur_switch_label;
      break;
    }
    case CONTINUE_STMT: {
      struct ContinueStmt* continue_stmt = &stmt->statement.continue_stmt;
      if (cur_loop_label == NULL) {
        label_error_at("Loop Labeling Error", stmt->loc,
                       "continue statement outside loop");
        return false;
      }
      // assign current label to continue statement
      continue_stmt->label = cur_loop_label;
      break;
    }
    case COMPOUND_STMT: {
      struct Block* block = stmt->statement.compound_stmt.block;
      if (!label_block(func_name, block)) {
        return false;
      }
      break;
    }
    case EXPR_STMT: {
      struct ExprStmt* expr_stmt = &stmt->statement.expr_stmt;
      if (expr_stmt->expr != NULL && !label_expr(func_name, expr_stmt->expr)) {
        return false;
      }
      break;
    }
    case IF_STMT: {
      struct IfStmt* if_stmt = &stmt->statement.if_stmt;

      if (!label_expr(func_name, if_stmt->condition)) {
        return false;
      }
      if (!label_stmt(func_name, if_stmt->if_stmt)) {
        return false;
      }
      if (if_stmt->else_stmt != NULL) {
        if (!label_stmt(func_name, if_stmt->else_stmt)) {
          return false;
        }
      }
      break;
    }
    case LABELED_STMT: {
      if (label_map_contains(goto_labels, stmt->statement.labeled_stmt.label)) {
        label_error_at1_slice("Loop Labeling Error", stmt->loc,
                              "multiple definitions for goto label %.*s",
                              stmt->statement.labeled_stmt.label);
        return false;
      }

      // Map user label -> unique label to avoid collisions across scopes.
      struct Slice* unique_label = make_unique_label(func_name, "goto");
      label_map_insert(goto_labels, stmt->statement.labeled_stmt.label, unique_label);

      stmt->statement.labeled_stmt.label = unique_label;

      struct LabeledStmt* labeled_stmt = &stmt->statement.labeled_stmt;
      if (!label_stmt(func_name, labeled_stmt->stmt)) {
        return false;
      }
      break;
    }
    case RETURN_STMT: {
      if (stmt->statement.ret_stmt.expr != NULL) {
        if (!label_expr(func_name, stmt->statement.ret_stmt.expr)) {
          return false;
        }
      }
      struct ReturnStmt* ret_stmt = &stmt->statement.ret_stmt;
      // assign function name as return label
      ret_stmt->func = func_name;
      break;
    }
    case CASE_STMT: {
      struct CaseStmt* case_stmt = &stmt->statement.case_stmt;

      if (cur_switch_label == NULL) {
        label_error_at("Loop Labeling Error", stmt->loc,
                       "case statement outside switch");
        return false;
      }

      if (case_stmt->expr->type != LIT) {
        label_error_at("Loop Labeling Error", stmt->loc,
                       "case statement with non-constant expression");
        return false;
      }

      // append ".case.num" to current switch label
      int num = case_stmt->expr->expr.lit_expr.value.int_val;
      case_stmt->label = make_case_label(cur_switch_label, num);

      if (!label_stmt(func_name, case_stmt->statement)) {
        return false;
      }
      break;
    }
    case DEFAULT_STMT: {
      struct DefaultStmt* default_stmt = &stmt->statement.default_stmt;
      if (cur_switch_label == NULL) {
        label_error_at("Loop Labeling Error", stmt->loc,
                       "default statement outside switch");
        return false;
      }

      // assign current switch label to default statement
      default_stmt->label = slice_concat(cur_switch_label, ".default");

      if (!label_stmt(func_name, default_stmt->statement)) {
        return false;
      }
      break;
    }
    default:
      // no labeling needed
      break;
  }

  return true;
}
    

// Purpose: Label each statement item inside a block.
// Inputs: func_name is the enclosing function name; block is the block list.
// Outputs: Returns true on success; false on any labeling error.
// Invariants/Assumptions: Initializers may contain statement expressions.
bool label_block(struct Slice* func_name, struct Block* block) {
  for (struct Block* item = block; item != NULL; item = item->next) {
    switch (item->item->type) {
      case STMT_ITEM:
        if (!label_stmt(func_name, item->item->item.stmt)) {
          return false;
        }
        break;
      case DCLR_ITEM:
        if (!label_local_dclr(func_name, item->item->item.dclr)) {
          return false;
        }
        break;
      default:
        label_error_at("Loop Labeling Error", NULL,
                       "unknown block item type");
        return false;
    }
  }
  return true;
}

// Purpose: Resolve goto labels inside expression subtrees with statement expressions.
// Inputs: expr is the expression to traverse.
// Outputs: Returns true on success; false on unresolved goto targets.
// Invariants/Assumptions: Statement expressions contain block lists.
static bool resolve_gotos_expr(struct Expr* expr) {
  if (expr == NULL) {
    return true;
  }

  switch (expr->type) {
    case BINARY:
      return resolve_gotos_expr(expr->expr.bin_expr.left) &&
             resolve_gotos_expr(expr->expr.bin_expr.right);
    case ASSIGN:
      return resolve_gotos_expr(expr->expr.assign_expr.left) &&
             resolve_gotos_expr(expr->expr.assign_expr.right);
    case POST_ASSIGN:
      return resolve_gotos_expr(expr->expr.post_assign_expr.expr);
    case CONDITIONAL:
      return resolve_gotos_expr(expr->expr.conditional_expr.condition) &&
             resolve_gotos_expr(expr->expr.conditional_expr.left) &&
             resolve_gotos_expr(expr->expr.conditional_expr.right);
    case CAST:
      return resolve_gotos_expr(expr->expr.cast_expr.expr);
    case FUNCTION_CALL: {
      if (!resolve_gotos_expr(expr->expr.fun_call_expr.func)) {
        return false;
      }
      for (struct ArgList* arg = expr->expr.fun_call_expr.args; arg != NULL; arg = arg->next) {
        if (!resolve_gotos_expr(arg->arg)) {
          return false;
        }
      }
      return true;
    }
    case ADDR_OF:
      return resolve_gotos_expr(expr->expr.addr_of_expr.expr);
    case DEREFERENCE:
      return resolve_gotos_expr(expr->expr.deref_expr.expr);
    case SUBSCRIPT:
      return resolve_gotos_expr(expr->expr.subscript_expr.array) &&
             resolve_gotos_expr(expr->expr.subscript_expr.index);
    case SIZEOF_EXPR:
      return resolve_gotos_expr(expr->expr.sizeof_expr.expr);
    case STMT_EXPR:
      return resolve_gotos(expr->expr.stmt_expr.block);
    case DOT_EXPR:
      return resolve_gotos_expr(expr->expr.dot_expr.struct_expr);
    case ARROW_EXPR:
      return resolve_gotos_expr(expr->expr.arrow_expr.pointer_expr);
    default:
      return true;
  }
}

// Purpose: Resolve goto labels inside initializer trees.
// Inputs: init is the initializer node to traverse.
// Outputs: Returns true on success; false on unresolved goto targets.
// Invariants/Assumptions: Compound initializers may nest arbitrarily.
static bool resolve_gotos_initializer(struct Initializer* init) {
  if (init == NULL) {
    return true;
  }

  switch (init->init_type) {
    case SINGLE_INIT:
      return resolve_gotos_expr(init->init.single_init);
    case COMPOUND_INIT:
      for (struct InitializerList* item = init->init.compound_init; item != NULL; item = item->next) {
        if (!resolve_gotos_initializer(item->init)) {
          return false;
        }
      }
      return true;
    default:
      label_error_at("Goto Resolution Error", init->loc,
                     "unknown initializer type");
      return false;
  }
}

// Purpose: Resolve goto labels inside a declaration initializer.
// Inputs: dclr is the declaration node to traverse.
// Outputs: Returns true on success; false on unresolved goto targets.
// Invariants/Assumptions: Local function declarations do not have bodies.
static bool resolve_gotos_dclr(struct Declaration* dclr) {
  switch (dclr->type) {
    case VAR_DCLR:
      return resolve_gotos_initializer(dclr->dclr.var_dclr.init);
    case FUN_DCLR:
      return true;
    case STRUCT_DCLR:
    case UNION_DCLR:
    case ENUM_DCLR:
      return true;
    default:
      label_error_at("Goto Resolution Error", NULL,
                     "unknown declaration type");
      return false;
  }
}

// Purpose: Resolve nested statements while rewriting goto targets.
// Inputs: stmt is the statement subtree to traverse.
// Outputs: Returns true on success; false if a goto target is missing.
// Invariants/Assumptions: goto_labels is populated for this function.
static bool resolve_stmt(struct Statement* stmt) {
  if (stmt->type == LABELED_STMT) {
    struct LabeledStmt* labeled_stmt = &stmt->statement.labeled_stmt;
    if (!resolve_stmt(labeled_stmt->stmt)) {
      return false;
    }
  } else if (stmt->type == COMPOUND_STMT) {
    struct Block* inner_block = stmt->statement.compound_stmt.block;
    if (!resolve_gotos(inner_block)) {
      return false;
    }
  } else if (stmt->type == IF_STMT) {
    struct IfStmt* if_stmt = &stmt->statement.if_stmt;
    if (!resolve_gotos_expr(if_stmt->condition)) {
      return false;
    }
    if (!resolve_stmt(if_stmt->if_stmt)) {
      return false;
    }
    if (if_stmt->else_stmt != NULL) {
      if (!resolve_stmt(if_stmt->else_stmt)) {
        return false;
      }
    }
  } else if (stmt->type == WHILE_STMT) {
    struct WhileStmt* while_stmt = &stmt->statement.while_stmt;
    if (!resolve_gotos_expr(while_stmt->condition)) {
      return false;
    }
    if (!resolve_stmt(while_stmt->statement)) {
      return false;
    }
  } else if (stmt->type == DO_WHILE_STMT) {
    struct DoWhileStmt* do_while_stmt = &stmt->statement.do_while_stmt;
    if (!resolve_stmt(do_while_stmt->statement)) {
      return false;
    }
    if (!resolve_gotos_expr(do_while_stmt->condition)) {
      return false;
    }
  } else if (stmt->type == FOR_STMT) {
    struct ForStmt* for_stmt = &stmt->statement.for_stmt;
    if (for_stmt->init != NULL) {
      if (for_stmt->init->type == DCLR_INIT) {
        if (!resolve_gotos_initializer(for_stmt->init->init.dclr_init->init)) {
          return false;
        }
      } else if (for_stmt->init->type == EXPR_INIT) {
        if (!resolve_gotos_expr(for_stmt->init->init.expr_init)) {
          return false;
        }
      } else {
        label_error_at("Goto Resolution Error", stmt->loc,
                       "unknown for initializer type");
        return false;
      }
    }
    if (!resolve_gotos_expr(for_stmt->condition)) {
      return false;
    }
    if (!resolve_gotos_expr(for_stmt->end)) {
      return false;
    }
    if (!resolve_stmt(for_stmt->statement)) {
      return false;
    }
  } else if (stmt->type == SWITCH_STMT) {
    struct SwitchStmt* switch_stmt = &stmt->statement.switch_stmt;
    if (!resolve_gotos_expr(switch_stmt->condition)) {
      return false;
    }
    if (!resolve_stmt(switch_stmt->statement)) {
      return false;
    }
  } else if (stmt->type == CASE_STMT) {
    struct CaseStmt* case_stmt = &stmt->statement.case_stmt;
    if (!resolve_gotos_expr(case_stmt->expr)) {
      return false;
    }
    if (!resolve_stmt(case_stmt->statement)) {
      return false;
    }
  } else if (stmt->type == DEFAULT_STMT) {
    struct DefaultStmt* default_stmt = &stmt->statement.default_stmt;
    if (!resolve_stmt(default_stmt->statement)) {
      return false;
    }
  } else if (stmt->type == RETURN_STMT) {
    struct ReturnStmt* ret_stmt = &stmt->statement.ret_stmt;
    if (!resolve_gotos_expr(ret_stmt->expr)) {
      return false;
    }
  } else if (stmt->type == EXPR_STMT) {
    struct ExprStmt* expr_stmt = &stmt->statement.expr_stmt;
    if (!resolve_gotos_expr(expr_stmt->expr)) {
      return false;
    }
  } else if (stmt->type == GOTO_STMT) {
    struct GotoStmt* goto_stmt = &stmt->statement.goto_stmt;
    // Replace user label with the unique label assigned during labeling.
    struct Slice* target_label = label_map_get(goto_labels, goto_stmt->label);
    if (target_label == NULL) {
      label_error_at1_slice("Goto Resolution Error", stmt->loc,
                            "label %.*s has no definition",
                            goto_stmt->label);
      return false;
    }
    goto_stmt->label = target_label;
  }

  return true;
}

// Purpose: Resolve goto statements within a block by replacing their labels.
// Inputs: block is the block list containing statements.
// Outputs: Returns true on success; false on unresolved labels.
// Invariants/Assumptions: Labeling pass created the goto label map.
bool resolve_gotos(struct Block* block) {
  for (struct Block* item = block; item != NULL; item = item->next) {
    // only need to resolve statements
    if (item->item->type == STMT_ITEM) {
      if (!resolve_stmt(item->item->item.stmt)) { 
        return false;
      }
    } else if (item->item->type == DCLR_ITEM) {
      if (!resolve_gotos_dclr(item->item->item.dclr)) {
        return false;
      }
    }
  }
  return true;
}

// -------------------------------- case collection -------------------------------- //

// Purpose: Traverse expressions to collect cases within statement expressions.
// Inputs: expr is the expression subtree to inspect.
// Outputs: Returns true on success; false on case collection errors.
// Invariants/Assumptions: Statement expressions contain block lists.
static bool collect_cases_expr(struct Expr* expr) {
  if (expr == NULL) {
    return true;
  }

  switch (expr->type) {
    case BINARY:
      return collect_cases_expr(expr->expr.bin_expr.left) &&
             collect_cases_expr(expr->expr.bin_expr.right);
    case ASSIGN:
      return collect_cases_expr(expr->expr.assign_expr.left) &&
             collect_cases_expr(expr->expr.assign_expr.right);
    case POST_ASSIGN:
      return collect_cases_expr(expr->expr.post_assign_expr.expr);
    case CONDITIONAL:
      return collect_cases_expr(expr->expr.conditional_expr.condition) &&
             collect_cases_expr(expr->expr.conditional_expr.left) &&
             collect_cases_expr(expr->expr.conditional_expr.right);
    case CAST:
      return collect_cases_expr(expr->expr.cast_expr.expr);
    case FUNCTION_CALL: {
      if (!collect_cases_expr(expr->expr.fun_call_expr.func)) {
        return false;
      }
      for (struct ArgList* arg = expr->expr.fun_call_expr.args; arg != NULL; arg = arg->next) {
        if (!collect_cases_expr(arg->arg)) {
          return false;
        }
      }
      return true;
    }
    case ADDR_OF:
      return collect_cases_expr(expr->expr.addr_of_expr.expr);
    case DEREFERENCE:
      return collect_cases_expr(expr->expr.deref_expr.expr);
    case SUBSCRIPT:
      return collect_cases_expr(expr->expr.subscript_expr.array) &&
             collect_cases_expr(expr->expr.subscript_expr.index);
    case SIZEOF_EXPR:
      return collect_cases_expr(expr->expr.sizeof_expr.expr);
    case STMT_EXPR:
      return collect_cases(expr->expr.stmt_expr.block);
    case DOT_EXPR: {
      return collect_cases_expr(expr->expr.dot_expr.struct_expr);
    }
    case ARROW_EXPR: {
      return collect_cases_expr(expr->expr.arrow_expr.pointer_expr);
    }
    default:
      return true;
  }
}

// Purpose: Collect cases within initializer trees that may contain statement expressions.
// Inputs: init is the initializer node to inspect.
// Outputs: Returns true on success; false on case collection errors.
// Invariants/Assumptions: Compound initializers may nest arbitrarily.
static bool collect_cases_initializer(struct Initializer* init) {
  if (init == NULL) {
    return true;
  }

  switch (init->init_type) {
    case SINGLE_INIT:
      return collect_cases_expr(init->init.single_init);
    case COMPOUND_INIT:
      for (struct InitializerList* item = init->init.compound_init; item != NULL; item = item->next) {
        if (!collect_cases_initializer(item->init)) {
          return false;
        }
      }
      return true;
    default:
      label_error_at("Case Collection Error", init->loc,
                     "unknown initializer type");
      return false;
  }
}

// Purpose: Collect cases within declaration initializers.
// Inputs: dclr is the declaration node to inspect.
// Outputs: Returns true on success; false on case collection errors.
// Invariants/Assumptions: Local function declarations do not have bodies.
static bool collect_cases_dclr(struct Declaration* dclr) {
  switch (dclr->type) {
    case VAR_DCLR:
      return collect_cases_initializer(dclr->dclr.var_dclr.init);
    case FUN_DCLR:
      return true;
    case STRUCT_DCLR:
    case UNION_DCLR:
    case ENUM_DCLR:
      return true;
    default:
      label_error_at("Case Collection Error", NULL,
                     "unknown declaration type");
      return false;
  }
}

// Purpose: Traverse statements to collect case/default labels per switch.
// Inputs: stmt is the statement subtree to inspect.
// Outputs: Returns true on success; false on duplicate/invalid cases.
// Invariants/Assumptions: current_case_list tracks the active switch.
bool collect_cases_stmt(struct Statement* stmt){
  switch (stmt->type) {
    case WHILE_STMT: {
      struct WhileStmt* while_stmt = &stmt->statement.while_stmt;
      if (!collect_cases_expr(while_stmt->condition)) {
        return false;
      }
      return collect_cases_stmt(while_stmt->statement);
    }
    case DO_WHILE_STMT: {
      struct DoWhileStmt* do_while_stmt = &stmt->statement.do_while_stmt;
      if (!collect_cases_expr(do_while_stmt->condition)) {
        return false;
      }
      return collect_cases_stmt(do_while_stmt->statement);
    }
    case FOR_STMT: {
      struct ForStmt* for_stmt = &stmt->statement.for_stmt;
      if (for_stmt->init != NULL) {
        if (for_stmt->init->type == DCLR_INIT) {
          if (!collect_cases_initializer(for_stmt->init->init.dclr_init->init)) {
            return false;
          }
        } else if (for_stmt->init->type == EXPR_INIT) {
          if (!collect_cases_expr(for_stmt->init->init.expr_init)) {
            return false;
          }
        } else {
          label_error_at("Case Collection Error", stmt->loc,
                         "unknown for initializer type");
          return false;
        }
      }
      if (!collect_cases_expr(for_stmt->condition)) {
        return false;
      }
      if (!collect_cases_expr(for_stmt->end)) {
        return false;
      }
      return collect_cases_stmt(for_stmt->statement);
    }
    case IF_STMT: {
      struct IfStmt* if_stmt = &stmt->statement.if_stmt;
      if (!collect_cases_expr(if_stmt->condition)) {
        return false;
      }
      if (!collect_cases_stmt(if_stmt->if_stmt)) {
        return false;
      }
      if (if_stmt->else_stmt != NULL) {
        if (!collect_cases_stmt(if_stmt->else_stmt)) {
          return false;
        }
      }
      return true;
    }
    case COMPOUND_STMT: {
      struct Block* block = stmt->statement.compound_stmt.block;
      return collect_cases(block);
    }
    case LABELED_STMT: {
      struct LabeledStmt* labeled_stmt = &stmt->statement.labeled_stmt;
      return collect_cases_stmt(labeled_stmt->stmt);
    }
    case CASE_STMT: {
      // extract case value
      struct CaseStmt* case_stmt = &stmt->statement.case_stmt;
      if (case_stmt->expr->type != LIT) {
        label_error_at("Case Collection Error", stmt->loc,
                       "case statement with non-constant expression");
        return false;
      }
      struct LitExpr* lit_expr = &case_stmt->expr->expr.lit_expr;
      int case_value = lit_expr->value.int_val;

      // Check for duplicate case values within the current switch.
      struct CaseList* case_iter = current_case_list;
      while (case_iter != NULL) {
        if (case_iter->case_label.type == INT_CASE && case_iter->case_label.data == case_value) {
          label_error_at1_int("Case Collection Error", stmt->loc,
                              "duplicate case %d", case_value);
          return false;
        }
        case_iter = case_iter->next;
      }

      // add case to current case list
      struct CaseList* new_case = arena_alloc(sizeof(struct CaseList));
      new_case->case_label.type = INT_CASE;
      new_case->case_label.data = case_value;
      new_case->next = current_case_list;
      current_case_list = new_case;

      return collect_cases_stmt(case_stmt->statement);
    }
    case DEFAULT_STMT: {
      // Ensure only one default per switch.
      struct CaseList* case_iter = current_case_list;
      while (case_iter != NULL) {
        if (case_iter->case_label.type == DEFAULT_CASE) {
          label_error_at("Case Collection Error", stmt->loc,
                         "duplicate default case");
          return false;
        }
        case_iter = case_iter->next;
      }
      struct DefaultStmt* default_stmt = &stmt->statement.default_stmt;
      // add default case to current case list
      struct CaseList* new_case = arena_alloc(sizeof(struct CaseList));
      new_case->case_label.type = DEFAULT_CASE;
      new_case->next = current_case_list;
      current_case_list = new_case;
      return collect_cases_stmt(default_stmt->statement);
    }
    case SWITCH_STMT: {
      // Each switch statement collects its own case list.
      struct CaseList* prev_case_list = current_case_list;
      current_case_list = NULL;

      if (!collect_cases_expr(stmt->statement.switch_stmt.condition)) {
        return false;
      }
      if (!collect_cases_stmt(stmt->statement.switch_stmt.statement)) {
        return false;
      }

      // assign collected cases to switch statement
      stmt->statement.switch_stmt.cases = current_case_list;

      current_case_list = prev_case_list;
      return true;
    }
    default:
      // no cases to collect
      return true;
  }
}

// Purpose: Collect case/default labels for every switch in a block.
// Inputs: block is the block list containing statements.
// Outputs: Returns true on success; false on case collection errors.
// Invariants/Assumptions: Case expressions must be literal integers.
bool collect_cases(struct Block* block) {
  for (struct Block* item = block; item != NULL; item = item->next) {
    // only need to process statements
    if (item->item->type == STMT_ITEM) {
      if (!collect_cases_stmt(item->item->item.stmt)) {
        return false;
      }
    } else if (item->item->type == DCLR_ITEM) {
      if (!collect_cases_dclr(item->item->item.dclr)) {
        return false;
      }
    }
  }
  return true;
}

// Purpose: Synthesize a unique label for a switch case value.
// Inputs: switch_label is the base switch label; case_value is the integer literal.
// Outputs: Returns a new Slice containing "switch.case.N".
// Invariants/Assumptions: Uses arena allocation for the label buffer.
struct Slice* make_case_label(struct Slice* switch_label, int case_value) {
  // append ".case.<u32>" to current switch label, using unsigned digits to keep labels valid
  uint32_t unsigned_value = (uint32_t)case_value;
  unsigned id_len = u32_len(unsigned_value);
  char* case_label_str = (char*)arena_alloc(switch_label->len + 6 + id_len); // len(".case.") == 6
  for (size_t i = 0; i < switch_label->len; i++) {
    case_label_str[i] = switch_label->start[i];
  }
  case_label_str[switch_label->len] = '.';
  case_label_str[switch_label->len + 1] = 'c';
  case_label_str[switch_label->len + 2] = 'a';
  case_label_str[switch_label->len + 3] = 's';
  case_label_str[switch_label->len + 4] = 'e';
  case_label_str[switch_label->len + 5] = '.';

  for (unsigned i = 0; i < id_len; i++) {
    case_label_str[switch_label->len + 6 + id_len - 1 - i] = '0' + (unsigned)(unsigned_value % 10u);
    unsigned_value /= 10u;
  }

  struct Slice* case_label = (struct Slice*)arena_alloc(sizeof(struct Slice));
  case_label->start = case_label_str;
  case_label->len = switch_label->len + 6 + id_len;

  return case_label;
}
