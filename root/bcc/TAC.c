#include "TAC.h"
#include "arena.h"
#include "label_resolution.h"
#include "source_location.h"
#include "unique_name.h"

#include "../crt/stdio.h"
#include "../crt/print.h"
#include "../crt/stdlib.h"
#include "../crt/limits.h"

static int tac_temp_counter = 0;
static int tac_label_counter = 0;

static bool debug_info_enabled = 0;

struct IdentAttr kLocalAttrs = {LOCAL_ATTR, true, NONE, {NO_INIT, NULL}};

// Purpose: Lower a compound array initializer into TAC CopyToOffset instructions.
// Inputs: func_name is the owning function; var_name is the array name.
// Outputs: Returns TAC instructions for array initialization.
// Invariants/Assumptions: type is ARRAY_TYPE and inits is size-padded.
static struct TACInstr* array_init_to_TAC(struct Slice* func_name, struct Slice* var_name,
    struct InitializerList* inits, struct Type* type, size_t offset);
static struct TACInstr* string_init_to_TAC(struct Slice* func_name, struct Slice* var_name,
    struct StringExpr* str_expr, struct Type* type, size_t offset);
static struct TACInstr* struct_init_to_TAC(struct Slice* func_name, struct Slice* base,
    struct InitializerList* inits, struct Type* type, size_t offset);
static int init_offset_to_int(size_t offset, char* loc);

// Purpose: Choose a source location pointer for a declaration.
// Inputs: dclr is the declaration to describe (may be NULL).
// Outputs: Returns a source pointer suitable for debug line markers.
// Invariants/Assumptions: Uses the initializer location when available.
static char* declaration_loc(struct Declaration* dclr) {
  if (dclr == NULL) {
    return NULL;
  }
  if (dclr->type == VAR_DCLR) {
    struct VariableDclr* var_dclr = &dclr->dclr.var_dclr;
    if (var_dclr->init != NULL && var_dclr->init->loc != NULL) {
      return var_dclr->init->loc;
    }
    if (var_dclr->name != NULL && var_dclr->name->start != NULL) {
      return var_dclr->name->start;
    }
  }
  return NULL;
}

// Purpose: Print the TAC error prefix, including source location when available.
// Inputs: loc may be NULL.
// Outputs: Writes the TAC error prefix to stdout.
// Invariants/Assumptions: Source context must be initialized for locations.
static void tac_error_prefix(char* loc) {
  struct SourceLocation where = source_location_from_ptr(loc);
  char* filename = source_filename_for_ptr(loc);

  if (where.line == 0) {
    fdputs(STDOUT, "TAC Error: ");
  } else {
    int args[3];

    args[0] = (int)filename;
    args[1] = (int)where.line;
    args[2] = (int)where.column;
    fdprintf(STDOUT, "TAC Error at %s:%zu:%zu: ", args);
  }
}

// Purpose: Print a TAC error without format arguments and exit.
// Inputs: loc may be NULL; message is the literal diagnostic text.
// Outputs: Writes the diagnostic to stdout and terminates with non-zero status.
// Invariants/Assumptions: message is a NUL-terminated string literal.
static void tac_error_at0(char* loc, char* message) {
  tac_error_prefix(loc);
  fdputs(STDOUT, message);
  fdputs(STDOUT, "\n");
  exit(EXIT_FAILURE);
}

// Purpose: Print a TAC error with one integer substitution and exit.
// Inputs: loc may be NULL; fmt contains one `%d` slot; arg0 is that value.
// Outputs: Writes the diagnostic to stdout and terminates with non-zero status.
// Invariants/Assumptions: fmt matches the single integer argument.
static void tac_error_at1_int(char* loc, char* fmt, int arg0) {
  int args[1];

  args[0] = arg0;
  tac_error_prefix(loc);
  fdprintf(STDOUT, fmt, args);
  fdputs(STDOUT, "\n");
  exit(EXIT_FAILURE);
}

// Purpose: Print a TAC error with one slice substitution and exit.
// Inputs: loc may be NULL; fmt contains one `%.*s` slot; slice names the object.
// Outputs: Writes the diagnostic to stdout and terminates with non-zero status.
// Invariants/Assumptions: slice is non-NULL and points to valid slice storage.
static void tac_error_at1_slice(char* loc, char* fmt, struct Slice* slice) {
  int args[2];

  args[0] = (int)slice->len;
  args[1] = (int)slice->start;
  tac_error_prefix(loc);
  fdprintf(STDOUT, fmt, args);
  fdputs(STDOUT, "\n");
  exit(EXIT_FAILURE);
}

// Purpose: Print a TAC error with two size substitutions and exit.
// Inputs: loc may be NULL; fmt contains two `%zu` slots.
// Outputs: Writes the diagnostic to stdout and terminates with non-zero status.
// Invariants/Assumptions: The sizes fit in Dioptase's 32-bit userland words.
static void tac_error_at2_size(char* loc, char* fmt, size_t arg0, size_t arg1) {
  int args[2];

  args[0] = (int)arg0;
  args[1] = (int)arg1;
  tac_error_prefix(loc);
  fdprintf(STDOUT, fmt, args);
  fdputs(STDOUT, "\n");
  exit(EXIT_FAILURE);
}

// Purpose: Allocate and initialize a single TAC instruction node.
// Inputs: type selects the instruction variant to populate later.
// Outputs: Returns a node with next == NULL and last == self.
// Invariants/Assumptions: Callers must fill the variant fields.
static struct TACInstr* tac_instr_create(enum TACInstrType type) {
  struct TACInstr* instr = (struct TACInstr*)arena_alloc(sizeof(struct TACInstr));
  instr->type = type;
  instr->next = NULL;
  instr->last = instr;
  return instr;
}

// Purpose: Find the last node in a TAC instruction list.
// Inputs: instr is the head of a TAC list.
// Outputs: Returns the final node (or NULL if instr is NULL).
// Invariants/Assumptions: List links are well-formed (acyclic).
static struct TACInstr* tac_find_last(struct TACInstr* instr) {
  if (instr == NULL) {
    return NULL;
  }
  struct TACInstr* cur = instr;
  while (cur->next != NULL) {
    cur = cur->next;
  }
  return cur;
}

// Purpose: Provide canonical scalar types for TAC constants.
// Inputs: kind is the scalar type to return.
// Outputs: Returns a stable Type pointer for the requested kind.
// Invariants/Assumptions: Only basic scalar types are cached here.
static struct Type* tac_builtin_type(enum TypeType kind) {
  static struct Type char_type = {CHAR_TYPE};
  static struct Type short_type = {SHORT_TYPE};
  static struct Type ushort_type = {USHORT_TYPE};
  static struct Type int_type = {INT_TYPE};
  static struct Type uint_type = {UINT_TYPE};
  static struct Type long_type = {LONG_TYPE};
  static struct Type ulong_type = {ULONG_TYPE};
  static struct Type pointer_type = {POINTER_TYPE};

  switch (kind) {
    case CHAR_TYPE:
      return &char_type;
    case SHORT_TYPE:
      return &short_type;
    case USHORT_TYPE:
      return &ushort_type;
    case INT_TYPE:
      return &int_type;
    case POINTER_TYPE:
      return &pointer_type;
    case UINT_TYPE:
      return &uint_type;
    case LONG_TYPE:
      return &long_type;
    case ULONG_TYPE:
      return &ulong_type;
    default:
      tac_error_at0(NULL, "unsupported builtin type kind in tac_builtin_type");
      exit(1);
  }
}

// Purpose: Allocate a constant TAC value.
// Inputs: value is the literal bits; type describes width/sign for conversions.
// Outputs: Returns a Val tagged as CONSTANT.
// Invariants/Assumptions: type is non-NULL for typed constants.
static struct Val* tac_make_const(uint64_t value, struct Type* type) {
  struct Val* val = (struct Val*)arena_alloc(sizeof(struct Val));
  val->val_type = CONSTANT;
  val->val.const_value = value;
  val->type = type;
  return val;
}

// Purpose: Allocate a variable TAC value referencing an existing name.
// Inputs: name is a Slice that must outlive the TAC; type is the variable type.
// Outputs: Returns a Val tagged as VARIABLE.
// Invariants/Assumptions: The Slice points to stable memory (arena or source).
static struct Val* tac_make_var(struct Slice* name, struct Type* type) {
  struct Val* val = (struct Val*)arena_alloc(sizeof(struct Val));
  val->val_type = VARIABLE;
  val->val.var_name = name;
  val->type = type;
  return val;
}

// Purpose: Copy a Val payload into a pre-allocated destination.
// Inputs: dst must be non-NULL; src must be non-NULL.
// Outputs: dst receives a shallow copy of src.
// Invariants/Assumptions: This does not deep-copy slices.
static void tac_copy_val(struct Val* dst, struct Val* src) {
  if (dst == NULL || src == NULL) {
    return;
  }
  *dst = *src;
}

// Purpose: Build a unique TAC label under the current function name.
// Inputs: func_name is the owning function; suffix differentiates labels.
// Outputs: Returns a new Slice for the label name.
// Invariants/Assumptions: Uses a monotonically increasing counter.
static struct Slice* tac_make_label(struct Slice* func_name, char* suffix) {
  size_t suffix_len = 0;
  while (suffix[suffix_len] != '\0') {
    suffix_len++;
  }
  unsigned id_len = counter_len(tac_label_counter);
  size_t new_len = func_name->len + 1 + suffix_len + 1 + id_len;

  char* new_str = (char*)arena_alloc(new_len);
  for (size_t i = 0; i < func_name->len; i++) {
    new_str[i] = func_name->start[i];
  }
  new_str[func_name->len] = '.';
  for (size_t i = 0; i < suffix_len; i++) {
    new_str[func_name->len + 1 + i] = suffix[i];
  }
  new_str[func_name->len + 1 + suffix_len] = '.';

  int id = tac_label_counter;
  for (unsigned i = 0; i < id_len; i++) {
    new_str[new_len - 1 - i] = '0' + (id % 10);
    id /= 10;
  }

  tac_label_counter++;

  struct Slice* unique_label = (struct Slice*)arena_alloc(sizeof(struct Slice));
  unique_label->start = new_str;
  unique_label->len = new_len;

  return unique_label;
}

// Purpose: Identify relational binary operators that yield boolean results.
// Inputs: op is the AST binary operator.
// Outputs: Returns true if op is a relational comparison.
// Invariants/Assumptions: Equality and ordering ops are the only relational ops.
static bool is_relational_op(enum BinOp op) {
  switch (op) {
    case BOOL_EQ:
    case BOOL_NEQ:
    case BOOL_LE:
    case BOOL_LEQ:
    case BOOL_GE:
    case BOOL_GEQ:
      return true;
    default:
      return false;
  }
}

// Purpose: Identify compound-assignment operators.
// Inputs: op is the AST binary operator.
// Outputs: Returns true for +=, -=, etc.
// Invariants/Assumptions: ASSIGN_OP is handled separately.
static bool is_compound_op(enum BinOp op) {
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

// Purpose: Map a compound-assignment operator to its binary operator.
// Inputs: op must satisfy is_compound_op(op).
// Outputs: Returns the underlying arithmetic/bitwise operator.
// Invariants/Assumptions: Caller must validate op.
static enum BinOp compound_to_binop(enum BinOp op) {
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

// Purpose: Map a relational operator to a TAC condition, using signedness.
// Inputs: op is a relational operator; type describes the operand type.
// Outputs: Returns the TAC condition used for a successful comparison.
// Invariants/Assumptions: type is arithmetic; signedness follows typechecking.
static enum TACCondition relation_to_cond(enum BinOp op, struct Type* type) {
  bool is_signed = is_signed_type(type);
  switch (op) {
    case BOOL_EQ:
      return CondE;
    case BOOL_NEQ:
      return CondNE;
    case BOOL_GE:
      return is_signed ? CondG : CondA;
    case BOOL_GEQ:
      return is_signed ? CondGE : CondAE;
    case BOOL_LE:
      return is_signed ? CondL : CondB;
    case BOOL_LEQ:
      return is_signed ? CondLE : CondBE;
    default:
      tac_error_at0(NULL, "invalid relational operator in relation_to_cond");
      return -1;
  }
}

// Purpose: Map a binary operator to an ALU op, using signedness.
// Inputs: op is a binary operator; type describes the operand type.
// Outputs: Returns the ALU op used
// Invariants/Assumptions: type is arithmetic; signedness follows typechecking.
static enum TACCondition binop_to_aluop(enum BinOp op, struct Type* type) {
  bool is_signed = is_signed_type(type);
  switch (op) {
    case ADD_OP:
      return ALU_ADD;
    case SUB_OP:
      return ALU_SUB;
    case MUL_OP:
      return is_signed ? ALU_SMUL : ALU_UMUL;
    case DIV_OP:
      return is_signed ? ALU_SDIV : ALU_UDIV;
    case MOD_OP:
      return is_signed ? ALU_SMOD : ALU_UMOD;
    case BIT_AND:
      return ALU_AND;
    case BIT_OR:
      return ALU_OR;
    case BIT_XOR:
      return ALU_XOR;
    case BIT_SHL:
      return is_signed ? ALU_ASL : ALU_LSL;
    case BIT_SHR:
      return is_signed ? ALU_ASR : ALU_LSR;
    case COMMA_OP:
      return ALU_MOV;
    default:
      tac_error_at0(NULL, "invalid binary operator in binop_to_aluop");
      return -1;
  }
}

// Purpose: Emit TAC for signed division/modulo when the LHS is a narrower unsigned type.
// Inputs: func_name scopes fresh labels; dst is the result lvalue; lhs/rhs are operands.
// Outputs: Returns a TAC instruction list that computes dst from lhs and rhs.
// Invariants/Assumptions: Handles only 32-bit operands, using unsigned ops plus sign fixes.
static struct TACInstr* emit_unsigned_lhs_signed_divmod(struct Slice* func_name,
                                                        struct Val* dst,
                                                        struct Val* lhs,
                                                        struct Val* rhs,
                                                        struct Type* rhs_type,
                                                        bool is_mod) {
  struct TACInstr* instrs = NULL;
  struct Val* rhs_abs = rhs;
  struct Val* rhs_neg = NULL;

  if (is_signed_type(rhs_type)) {
    rhs_abs = make_temp(func_name, rhs_type);
    rhs_neg = make_temp(func_name, tac_builtin_type(INT_TYPE));

    struct Slice* rhs_nonneg = tac_make_label(func_name, "rhs_nonneg");
    struct Slice* rhs_done = tac_make_label(func_name, "rhs_done");

    struct TACInstr* cmp_rhs = tac_instr_create(TACCMP);
    cmp_rhs->instr.tac_cmp.src1 = rhs;
    cmp_rhs->instr.tac_cmp.src2 = tac_make_const(0, rhs_type);
    concat_TAC_instrs(&instrs, cmp_rhs);

    struct TACInstr* jump_rhs_nonneg = tac_instr_create(TACCOND_JUMP);
    jump_rhs_nonneg->instr.tac_cond_jump.condition = CondGE;
    jump_rhs_nonneg->instr.tac_cond_jump.label = rhs_nonneg;
    concat_TAC_instrs(&instrs, jump_rhs_nonneg);

    struct TACInstr* negate_rhs = tac_instr_create(TACUNARY);
    negate_rhs->instr.tac_unary.op = NEGATE;
    negate_rhs->instr.tac_unary.dst = rhs_abs;
    negate_rhs->instr.tac_unary.src = rhs;
    concat_TAC_instrs(&instrs, negate_rhs);

    struct TACInstr* rhs_neg_true = tac_instr_create(TACCOPY);
    rhs_neg_true->instr.tac_copy.dst = rhs_neg;
    rhs_neg_true->instr.tac_copy.src = tac_make_const(1, rhs_neg->type);
    concat_TAC_instrs(&instrs, rhs_neg_true);

    struct TACInstr* jump_rhs_done = tac_instr_create(TACJUMP);
    jump_rhs_done->instr.tac_jump.label = rhs_done;
    concat_TAC_instrs(&instrs, jump_rhs_done);

    struct TACInstr* rhs_nonneg_label = tac_instr_create(TACLABEL);
    rhs_nonneg_label->instr.tac_label.label = rhs_nonneg;
    concat_TAC_instrs(&instrs, rhs_nonneg_label);

    struct TACInstr* rhs_copy = tac_instr_create(TACCOPY);
    rhs_copy->instr.tac_copy.dst = rhs_abs;
    rhs_copy->instr.tac_copy.src = rhs;
    concat_TAC_instrs(&instrs, rhs_copy);

    struct TACInstr* rhs_neg_false = tac_instr_create(TACCOPY);
    rhs_neg_false->instr.tac_copy.dst = rhs_neg;
    rhs_neg_false->instr.tac_copy.src = tac_make_const(0, rhs_neg->type);
    concat_TAC_instrs(&instrs, rhs_neg_false);

    struct TACInstr* rhs_done_label = tac_instr_create(TACLABEL);
    rhs_done_label->instr.tac_label.label = rhs_done;
    concat_TAC_instrs(&instrs, rhs_done_label);
  }

  struct TACInstr* bin_instr = tac_instr_create(TACBINARY);
  bin_instr->instr.tac_binary.alu_op = is_mod ? ALU_UMOD : ALU_UDIV;
  bin_instr->instr.tac_binary.dst = dst;
  bin_instr->instr.tac_binary.src1 = lhs;
  bin_instr->instr.tac_binary.src2 = rhs_abs;
  concat_TAC_instrs(&instrs, bin_instr);

  if (!is_mod && rhs_neg != NULL) {
    struct Slice* div_done = tac_make_label(func_name, "div_done");

    struct TACInstr* cmp_neg = tac_instr_create(TACCMP);
    cmp_neg->instr.tac_cmp.src1 = rhs_neg;
    cmp_neg->instr.tac_cmp.src2 = tac_make_const(0, rhs_neg->type);
    concat_TAC_instrs(&instrs, cmp_neg);

    struct TACInstr* jump_done = tac_instr_create(TACCOND_JUMP);
    jump_done->instr.tac_cond_jump.condition = CondE;
    jump_done->instr.tac_cond_jump.label = div_done;
    concat_TAC_instrs(&instrs, jump_done);

    struct TACInstr* negate_dst = tac_instr_create(TACUNARY);
    negate_dst->instr.tac_unary.op = NEGATE;
    negate_dst->instr.tac_unary.dst = dst;
    negate_dst->instr.tac_unary.src = dst;
    concat_TAC_instrs(&instrs, negate_dst);

    struct TACInstr* div_done_label = tac_instr_create(TACLABEL);
    div_done_label->instr.tac_label.label = div_done;
    concat_TAC_instrs(&instrs, div_done_label);
  }

  return instrs;
}

// Purpose: Allocate a new temporary variable for TAC lowering.
// Inputs: func_name identifies the owning function; type documents the value type.
// Outputs: Returns a Val naming the temporary.
// Invariants/Assumptions: The temp counter is global and monotonically increasing.
struct Val* make_temp(struct Slice* func_name, struct Type* type) {
  if (type->type == VOID_TYPE) {
    // no destination for void type
    return NULL;
  }

  unsigned id_len = counter_len(tac_temp_counter);
  size_t new_len = func_name->len + 5 + id_len; // len(".tmp.") == 5

  char* new_str = (char*)arena_alloc(new_len);
  for (size_t i = 0; i < func_name->len; i++) {
    new_str[i] = func_name->start[i];
  }
  new_str[func_name->len] = '.';
  new_str[func_name->len + 1] = 't';
  new_str[func_name->len + 2] = 'm';
  new_str[func_name->len + 3] = 'p';
  new_str[func_name->len + 4] = '.';

  // append unique id
  int id = tac_temp_counter;
  for (unsigned i = 0; i < id_len; i++) {
    new_str[new_len - 1 - i] = '0' + (id % 10);
    id /= 10;
  }

  tac_temp_counter++;

  struct Slice* unique_label = (struct Slice*)arena_alloc(sizeof(struct Slice));
  unique_label->start = new_str;
  unique_label->len = new_len;

  struct Val* val = (struct Val*)arena_alloc(sizeof(struct Val));
  val->val_type = VARIABLE;
  val->val.var_name = unique_label;
  val->type = type;

  // add entry to symbol table
  symbol_table_insert(global_symbol_table, unique_label, type, &kLocalAttrs);

  return val;
}

struct Val* make_str_label(struct StringExpr* str_expr){
  struct Slice name_slice = {"string.label", 12};
  struct Slice* string_label = make_unique(&name_slice);

  struct Val* val = (struct Val*)arena_alloc(sizeof(struct Val));
  val->val_type = VARIABLE;
  val->val.var_name = string_label;
  val->type = arena_alloc(sizeof(struct Type));
  val->type->type = POINTER_TYPE;
  val->type->type_data.pointer_type.referenced_type = tac_builtin_type(CHAR_TYPE);
  return val;
}

// Purpose: Lower a full program into a TAC program containing top-level items.
// Inputs: program is a fully labeled and typechecked AST.
// Outputs: Returns a TAC program with a linked list of TopLevel entries.
// Invariants/Assumptions: Program declarations are in source order.
struct TACProg* prog_to_TAC(struct Program* program, bool emit_debug_info) {
  debug_info_enabled = emit_debug_info;

  struct TACProg* tac_prog = (struct TACProg*)arena_alloc(sizeof(struct TACProg));
  tac_prog->head = NULL;
  tac_prog->tail = NULL;
  tac_prog->statics = NULL;

  for (struct DeclarationList* decl = program->dclrs; decl != NULL; decl = decl->next) {
    struct TopLevel* top_level = file_scope_dclr_to_TAC(&decl->dclr);

    // append TopLevel to TACProg
    if (top_level != NULL) {
      if (tac_prog->head == NULL) {
        tac_prog->head = top_level;
        tac_prog->tail = top_level;
      } else {
        tac_prog->tail->next = top_level;
        tac_prog->tail = top_level;
      }
    }
  }

  // Collect static storage entries from the symbol table for visibility.
  struct TopLevel* statics_head = NULL;
  struct TopLevel* statics_tail = NULL;
  for (size_t i = 0; i < global_symbol_table->size; i++) {
    for (struct SymbolEntry* entry = global_symbol_table->arr[i];
      entry != NULL;
      entry = entry->next) {
      struct TopLevel* top_level = symbol_to_TAC(entry);
      if (top_level != NULL) {
        if (statics_head == NULL) {
          statics_head = top_level;
          statics_tail = top_level;
        } else {
          statics_tail->next = top_level;
          statics_tail = top_level;
        }
      }
    }
  }
  tac_prog->statics = statics_head;

  return tac_prog;
}

// Purpose: Lower a file-scope declaration into a TopLevel TAC node.
// Inputs: declaration points to a parsed, typechecked declaration.
// Outputs: Returns a TopLevel node or NULL if no TAC is generated.
// Invariants/Assumptions: Non-function definitions are handled via the symbol table.
struct TopLevel* file_scope_dclr_to_TAC(struct Declaration* declaration) {
  switch (declaration->type) {
    case FUN_DCLR:
      return func_to_TAC(&declaration->dclr.fun_dclr);
    case VAR_DCLR:
    case STRUCT_DCLR:
    case UNION_DCLR:
    case ENUM_DCLR:
      // TAC for static variables is generated from the symbol table later.
      // struct/union/enum declarations do not produce TAC.
      return NULL;
    default:
      tac_error_at0(NULL, "invalid declaration type in file scope");
      return NULL;
  }
}

// Purpose: Lower a symbol table entry into a TopLevel static variable node.
// Inputs: symbol is a file-scope entry from the global symbol table.
// Outputs: Returns a TopLevel node or NULL if not a static variable.
// Invariants/Assumptions: Only STATIC_ATTR entries produce TAC output.
struct TopLevel* symbol_to_TAC(struct SymbolEntry* symbol) {
  switch (symbol->attrs->attr_type) {
    case FUN_ATTR:
      return NULL;
    case STATIC_ATTR: {
      if (symbol->attrs->storage == EXTERN &&
          symbol->attrs->init.init_type == NO_INIT) {
        // Pure extern declaration; no TAC generated.
        return NULL;
      }

      struct TopLevel* top_level = (struct TopLevel*)arena_alloc(sizeof(struct TopLevel));
      top_level->type = STATIC_VAR;
      top_level->name = symbol->key;
      top_level->global = symbol->attrs->storage != STATIC;

      top_level->var_type = symbol->type;
      top_level->init_values = symbol->attrs->init.init_list;
      if (symbol->attrs->init.init_type == INITIAL && symbol->attrs->init.init_list != NULL) {
        size_t count = 0;
        for (struct InitList* init = symbol->attrs->init.init_list;
             init != NULL;
             init = init->next) {
          count++;
        }
      }
      
      top_level->next = NULL;
      return top_level;
    }
    case LOCAL_ATTR:
      // local variables do not produce file-scope TAC
      return NULL;
    case CONST_ATTR: {
      struct TopLevel* top_level = (struct TopLevel*)arena_alloc(sizeof(struct TopLevel));
      top_level->type = STATIC_CONST;
      top_level->name = symbol->key;
      top_level->global = false;
      top_level->var_type = symbol->type;
      top_level->init_values = symbol->attrs->init.init_list;
      top_level->next = NULL;
      return top_level;
    }
    default:
      tac_error_at0(NULL, "invalid symbol attribute type in file scope");
      return NULL;
  }
}

// Purpose: Lower a function definition into a TopLevel TAC node.
// Inputs: declaration points to a function declaration with an optional body.
// Outputs: Returns a TopLevel node or NULL for declarations without a body.
// Invariants/Assumptions: The symbol table entry must already exist.
struct TopLevel* func_to_TAC(struct FunctionDclr* declaration) {
  if (declaration->body == NULL) {
    // function declaration without body; return NULL
    return NULL;
  }

  struct TACInstr* body = block_to_TAC(declaration->name, declaration->body);
  struct SymbolEntry* symbol = symbol_table_get(global_symbol_table, declaration->name);
  if (symbol == NULL || symbol->attrs->attr_type != FUN_ATTR) {
    tac_error_at0(declaration->name ? declaration->name->start : NULL,
                  "missing or invalid symbol table entry for function");
    return NULL;
  }

  struct TopLevel* top_level = (struct TopLevel*)arena_alloc(sizeof(struct TopLevel));
  top_level->type = FUNC;
  top_level->name = declaration->name;
  top_level->global = symbol->attrs->storage != STATIC;

  top_level->body = body;

  // collect parameter names
  size_t num_params = 0;
  for (struct ParamList* param = declaration->params; param != NULL; param = param->next) {
    num_params++;
  }
  top_level->num_params = num_params;
  top_level->params = (struct Slice**)arena_alloc(sizeof(struct Slice*) * num_params);
  size_t i = 0;
  for (struct ParamList* param = declaration->params; param != NULL; param = param->next, i++) {
    top_level->params[i] = param->param.name;
  }
  top_level->next = NULL;

  // append return instruction in case function does not end with return
  // AST:
  // (implicit return at function end)
  // TAC:
  // Return Const(0)
  struct TACInstr* ret_instr = tac_instr_create(TACRETURN);
  ret_instr->instr.tac_return.dst = tac_make_const(0, tac_builtin_type(INT_TYPE)); // default return 0

  concat_TAC_instrs(&top_level->body, ret_instr);

  return top_level;
}

// Purpose: Lower a block of statements/declarations into a TAC instruction list.
// Inputs: func_name is the owning function; block is the linked list of items.
// Outputs: Returns the head of the TAC instruction list (or NULL if empty).
// Invariants/Assumptions: Block items are lowered in source order.
struct TACInstr* block_to_TAC(struct Slice* func_name, struct Block* block) {
  struct TACInstr* head = NULL;

  for (struct Block* cur = block; cur != NULL; cur = cur->next) {
    struct TACInstr* item_instrs = NULL;
    switch (cur->item->type) {
      case STMT_ITEM:
        if (debug_info_enabled) {
          struct TACInstr* boundary_before = tac_instr_create(TACBOUNDARY);
          boundary_before->instr.tac_boundary.loc = cur->item->item.stmt->loc;
          concat_TAC_instrs(&item_instrs, boundary_before);
        }
        concat_TAC_instrs(&item_instrs, stmt_to_TAC(func_name, cur->item->item.stmt));
        break;
      case DCLR_ITEM:
        item_instrs = local_dclr_to_TAC(func_name, cur->item->item.dclr);
        if (debug_info_enabled) {
          char* loc = declaration_loc(cur->item->item.dclr);
          if (loc != NULL) {
            struct TACInstr* boundary_dclr = tac_instr_create(TACBOUNDARY);
            boundary_dclr->instr.tac_boundary.loc = loc;
            concat_TAC_instrs(&boundary_dclr, item_instrs);
            item_instrs = boundary_dclr;
          }
        }
        break;
      default:
        tac_error_at0(NULL, "invalid block item type");
        return NULL;
    }

    concat_TAC_instrs(&head, item_instrs);
  }

  // call all cleanup handlers for variables going out of scope
  if (block == NULL || block->idents == NULL) {
    return head;
  }
  for (size_t i = 0; i < block->idents->size; ++i){
    for (struct IdentMapEntry* ident_entry = block->idents->arr[i]; ident_entry != NULL; ident_entry = ident_entry->next) {
      struct SymbolEntry* symbol_entry = symbol_table_get(global_symbol_table, ident_entry->entry_name);
      if (symbol_entry == NULL || symbol_entry->attrs == NULL) {
        continue;
      }
      if (symbol_entry->attrs->attr_type == LOCAL_ATTR &&
          symbol_entry->attrs->cleanup_handler != NULL) {
        struct TACInstr* cleanup_instr = tac_instr_create(TACCALL);
        cleanup_instr->instr.tac_call.func_name = symbol_entry->attrs->cleanup_handler;
        struct Val* var = tac_make_var(ident_entry->entry_name, symbol_entry->type);
        struct Val* addr = make_temp(func_name, tac_builtin_type(UINT_TYPE));
        struct TACInstr* addr_instr = tac_instr_create(TACGET_ADDRESS);
        addr_instr->instr.tac_get_address.dst = addr;
        addr_instr->instr.tac_get_address.src = var;
        concat_TAC_instrs(&head, addr_instr);
        cleanup_instr->instr.tac_call.args = addr;
        cleanup_instr->instr.tac_call.dst = NULL;
        cleanup_instr->instr.tac_call.num_args = 1;
        concat_TAC_instrs(&head, cleanup_instr);
      }
    }
  }
  if (block->idents != NULL) {
    destroy_ident_map(block->idents);
    block->idents = NULL;
  }

  return head;
}

// Purpose: Lower a local declaration into TAC instructions.
// Inputs: func_name is the owning function; dclr is a local declaration.
// Outputs: Returns a TAC list for initialization, or NULL if no code emitted.
// Invariants/Assumptions: Local function definitions are rejected earlier.
struct TACInstr* local_dclr_to_TAC(struct Slice* func_name, struct Declaration* dclr) {
  switch (dclr->type) {
    case VAR_DCLR:
      return var_dclr_to_TAC(func_name, dclr);
    case FUN_DCLR:
      if (dclr->dclr.fun_dclr.body == NULL) {
        // function declaration without body is okay; return NULL
        return NULL;
      }
      // local function definitions should have been caught by now
      tac_error_at0(NULL, "local function definition reached TAC lowering");
      return NULL;
    case STRUCT_DCLR:
    case UNION_DCLR:
    case ENUM_DCLR:
      // Local tag declarations only affect the type namespace.
      return NULL;
    default:
      tac_error_at0(NULL, "invalid declaration type in local scope");
      return NULL;
  }
}

struct TACInstr* compound_init_to_TAC(struct Slice* func_name,
                                      struct Slice* base_var,
                                      struct InitializerList* inits,
                                      struct Type* type,
                                      size_t offset) {
  if (type->type == ARRAY_TYPE) {
    return array_init_to_TAC(func_name, base_var, inits,
                           type, offset);
  } else if (type->type == STRUCT_TYPE ||
             type->type == UNION_TYPE) {
    return struct_init_to_TAC(func_name, base_var, inits, type, offset);
  } else {
    tac_error_at0(inits->init->loc,
                  "compound initializer for non-aggregate local variable");
    return NULL;
  }
}

// Purpose: Lower a single initializer for a scalar or aggregate element.
// Inputs: func_name is the owning function; base_var is the target variable name.
// Outputs: Returns TAC instructions that perform the initialization.
// Invariants/Assumptions: offset is a byte offset into base_var storage.
struct TACInstr* single_init_to_TAC(struct Slice* func_name,
                                    struct Slice* base_var,
                                    struct Expr* init,
                                    struct Type* type,
                                    size_t offset) {
  if (type != NULL && type->type == ARRAY_TYPE && init->type == STRING) {
    return string_init_to_TAC(func_name, base_var,
                              &init->expr.string_expr,
                              type, offset);
  }

  bool use_offset_store = offset != 0;
  struct SymbolEntry* base_entry = symbol_table_get(global_symbol_table, base_var);
  if (base_entry != NULL && base_entry->type != NULL) {
    enum TypeType base_kind = base_entry->type->type;
    if (base_kind == ARRAY_TYPE || base_kind == STRUCT_TYPE || base_kind == UNION_TYPE) {
      // was an aggregate type; must use offset store
      use_offset_store = true;
    }
  }
  if (use_offset_store) {
    struct Val* src = (struct Val*)arena_alloc(sizeof(struct Val));
    struct TACInstr* expr_instrs = expr_to_TAC_convert(func_name, init, src);
    struct TACInstr* store_instr = tac_instr_create(TACCOPY_TO_OFFSET);
    store_instr->instr.tac_copy_to_offset.dst = base_var;
    store_instr->instr.tac_copy_to_offset.src = src;
    store_instr->instr.tac_copy_to_offset.offset =
        init_offset_to_int(offset, init->loc);
    store_instr->instr.tac_copy_to_offset.dst_type = type;
    concat_TAC_instrs(&expr_instrs, store_instr);
    return expr_instrs;
  }

  // was not an aggregate type; use direct assignment

  struct Expr assign_expr;
  assign_expr.type = ASSIGN;
  assign_expr.loc = init->loc;
  assign_expr.value_type = type;

  struct Expr var_expr;
  var_expr.type = VAR;
  var_expr.loc = init->loc;
  var_expr.value_type = type;
  var_expr.expr.var_expr.name = base_var;

  assign_expr.expr.assign_expr.left = &var_expr;
  assign_expr.expr.assign_expr.right = init;

  return expr_to_TAC_convert(func_name, &assign_expr, NULL);
}

struct TACInstr* init_to_TAC(struct Slice* func_name,
                                 struct Slice* base_var,
                                 struct Initializer* init,
                                 struct Type* type,
                                 size_t offset) {
  switch (init->init_type) {
    case SINGLE_INIT: {
      return single_init_to_TAC(func_name, base_var,
                                init->init.single_init,
                                type, offset);
    }
    case COMPOUND_INIT: {
      return compound_init_to_TAC(func_name, base_var,
                                 init->init.compound_init,
                                 type, offset);
    }
    default:
      tac_error_at0(init->loc, "invalid initializer type in init_to_TAC");
      return NULL;
  }
}

// Purpose: Lower a local variable declaration initializer into TAC.
// Inputs: func_name is the owning function; dclr is a variable declaration.
// Outputs: Returns TAC instructions for the initializer, or NULL if none.
// Invariants/Assumptions: Static initializers are handled at file scope.
struct TACInstr* var_dclr_to_TAC(struct Slice* func_name, struct Declaration* dclr) {
  struct VariableDclr* var_dclr = &dclr->dclr.var_dclr;
  if (var_dclr->init == NULL) {
    // no initialization; return NULL
    return NULL;
  }

  switch (var_dclr->storage) {
    case STATIC:
      // static variable initialization is handled in the TopLevel generation
      return NULL;
    case NONE: {
      // local variable initialization

      // create assignment statement and convert to TAC
      return init_to_TAC(func_name, var_dclr->name,
                         var_dclr->init,
                         var_dclr->type,
                         0);
    }
    default:
      tac_error_at0(var_dclr->name ? var_dclr->name->start : NULL,
                    "invalid storage class for local variable declaration");
      return NULL;
  }
}

// Purpose: Normalize initializer offsets to the TAC int field width.
// Inputs: offset is a byte offset; loc is the initializer location for errors.
// Outputs: Returns the offset as an int for TAC encodings.
// Invariants/Assumptions: Offsets must fit in int for the TAC representation.
static int init_offset_to_int(size_t offset, char* loc) {
  if (offset > (size_t)INT_MAX) {
    tac_error_at0(loc, "initializer offset too large");
  }
  return (int)offset;
}

// Purpose: Lower a string literal initializer for a local char array into TAC stores.
// Inputs: var_name is the array name; str_expr is the literal.
// Outputs: Returns TAC instructions that write the string bytes into the array storage.
// Invariants/Assumptions: type is ARRAY_TYPE with a char-like element type.
static struct TACInstr* string_init_to_TAC(struct Slice* func_name, struct Slice* var_name,
                                           struct StringExpr* str_expr, struct Type* type, size_t offset) {
  if (type == NULL || type->type != ARRAY_TYPE) {
    tac_error_at0(var_name ? var_name->start : NULL, "string init requires array type");
    return NULL;
  }

  if (type == NULL || type->type != ARRAY_TYPE) {
    tac_error_at0(NULL, "string init requires array type");
    return NULL;
  }
  if (str_expr == NULL || str_expr->string == NULL) {
    tac_error_at0(NULL, "missing string literal in array init");
    return NULL;
  }

  struct Type* element_type = type->type_data.array_type.element_type;
  if (!is_char_type(element_type)) {
    tac_error_at0(NULL, "string init requires char array element type");
    return NULL;
  }

  size_t array_len = type->type_data.array_type.size;
  size_t str_len = str_expr->string->len;
  if (str_len > array_len) {
    tac_error_at0(NULL, "string initializer too long for array");
    return NULL;
  }

  struct TACInstr* instrs = NULL;
  size_t element_size = get_type_size(element_type);
  for (size_t i = 0; i < array_len; i++) {
    uint64_t byte = 0;
    if (i < str_len) {
      byte = (uint64_t)(unsigned char)str_expr->string->start[i];
    }

    struct Val* src = tac_make_const(byte, element_type);
    size_t byte_offset = offset + (i * element_size);
    struct TACInstr* store_instr = tac_instr_create(TACCOPY_TO_OFFSET);
    store_instr->instr.tac_copy_to_offset.dst = var_name;
    store_instr->instr.tac_copy_to_offset.src = src;
    store_instr->instr.tac_copy_to_offset.offset =
        init_offset_to_int(byte_offset, str_expr->string->start);
    store_instr->instr.tac_copy_to_offset.dst_type = element_type;
    concat_TAC_instrs(&instrs, store_instr);
  }

  return instrs;
}

static struct TACInstr* struct_init_to_TAC(struct Slice* func_name, struct Slice* base,
                                           struct InitializerList* inits,
                                           struct Type* type,
                                           size_t offset){
  if (inits == NULL) {
    return NULL;
  }
  if (type == NULL || (type->type != STRUCT_TYPE && type->type != UNION_TYPE)) {
    tac_error_at0(base ? base->start : NULL, "struct init requires struct/union type");
    return NULL;  
  }
  struct TACInstr* instrs = NULL;
  struct TypeEntry* type_entry = type_table_get(global_type_table, type->type_data.struct_type.name);
  struct MemberEntry* members = type_entry->data.struct_entry->members;
  struct InitializerList* cur_init = inits;

  for (struct MemberEntry* cur_member = members; cur_member != NULL && cur_init != NULL; cur_member = cur_member->next, cur_init = cur_init->next) {
    size_t mem_offset = offset + cur_member->offset;
    concat_TAC_instrs(&instrs, init_to_TAC(func_name, base, cur_init->init, cur_member->type, mem_offset));
  }
  return instrs;
}

// Purpose: Lower a compound array initializer into TAC CopyToOffset instructions.
// Inputs: func_name is the owning function; var_name is the array name.
// Outputs: Returns TAC instructions for array initialization.
// Invariants/Assumptions: type is ARRAY_TYPE and inits is size-padded.
static struct TACInstr* array_init_to_TAC(struct Slice* func_name, struct Slice* var_name,
    struct InitializerList* inits, struct Type* type, size_t offset) {
  if (inits == NULL) {
    return NULL;
  }
  if (type == NULL || type->type != ARRAY_TYPE) {
    tac_error_at0(var_name ? var_name->start : NULL, "array init requires array type");
    return NULL;
  }

  if (inits == NULL) {
    return NULL;
  }
  if (type == NULL || type->type != ARRAY_TYPE) {
    tac_error_at0(NULL, "array init requires array type");
    return NULL;
  }

  struct Type* element_type = type->type_data.array_type.element_type;
  size_t element_size = get_type_size(element_type);
  size_t cur_offset = offset;
  struct TACInstr* instrs = NULL;

  for (struct InitializerList* cur = inits; cur != NULL; cur = cur->next) {
    if (cur->init == NULL) {
      tac_error_at0(NULL, "missing initializer in array init");
      return instrs;
    }

    concat_TAC_instrs(&instrs, init_to_TAC(func_name, var_name, cur->init, element_type, cur_offset));

    cur_offset += element_size;
  }

  return instrs;
}

// Purpose: Lower a statement into a TAC instruction list.
// Inputs: func_name is the owning function; stmt is a typechecked statement.
// Outputs: Returns the TAC list for the statement, or NULL for empty statements.
// Invariants/Assumptions: Loop/switch labels are resolved before lowering.
struct TACInstr* stmt_to_TAC(struct Slice* func_name, struct Statement* stmt) {
  switch (stmt->type) {
    case RETURN_STMT: {
      // AST:
      // return expr
      // TAC:
      // <expr>
      // Return dst
      struct Val* dst = NULL;
      struct TACInstr* expr_instrs = NULL;
      
      if (stmt->statement.ret_stmt.expr != NULL){
        dst = (struct Val*)arena_alloc(sizeof(struct Val));
        expr_instrs = expr_to_TAC_convert(func_name, stmt->statement.ret_stmt.expr, dst);
      }

      struct TACInstr* ret_instr = tac_instr_create(TACRETURN);
      ret_instr->instr.tac_return.dst = dst;
      
      concat_TAC_instrs(&expr_instrs, ret_instr);
      return expr_instrs;
    }
    case EXPR_STMT:
      return expr_to_TAC_convert(func_name, stmt->statement.expr_stmt.expr, NULL);
    case IF_STMT: {
      if (stmt->statement.if_stmt.else_stmt != NULL) {
        // if-else statement
        return if_else_to_TAC(func_name, stmt->statement.if_stmt.condition,
                              stmt->statement.if_stmt.if_stmt,
                              stmt->statement.if_stmt.else_stmt);
      } else {
        // if statement without else
        return if_to_TAC(func_name, stmt->statement.if_stmt.condition, stmt->statement.if_stmt.if_stmt);
      }
    }
    case GOTO_STMT: {
      // AST:
      // goto label
      // TAC:
      // Jump label
      struct TACInstr* jump_instr = tac_instr_create(TACJUMP);
      jump_instr->instr.tac_jump.label = stmt->statement.goto_stmt.label;

      return jump_instr;
    }
    case LABELED_STMT: {
      // AST:
      // label: stmt
      // TAC:
      // Label label
      // <stmt>
      struct TACInstr* stmt_instrs = stmt_to_TAC(func_name, stmt->statement.labeled_stmt.stmt);

      struct TACInstr* label_instr = tac_instr_create(TACLABEL);
      label_instr->instr.tac_label.label = stmt->statement.labeled_stmt.label;

      concat_TAC_instrs(&label_instr, stmt_instrs);
      return label_instr;
    }
    case COMPOUND_STMT: {
      return block_to_TAC(func_name, stmt->statement.compound_stmt.block);
    }
    case BREAK_STMT: {
      // AST:
      // break
      // TAC:
      // Jump label.break
      struct TACInstr* jump_instr = tac_instr_create(TACJUMP);
      jump_instr->instr.tac_jump.label = slice_concat(stmt->statement.break_stmt.label, ".break");

      return jump_instr;
    }
    case CONTINUE_STMT: {
      // AST:
      // continue
      // TAC:
      // Jump label.continue
      struct TACInstr* jump_instr = tac_instr_create(TACJUMP);
      jump_instr->instr.tac_jump.label = slice_concat(stmt->statement.continue_stmt.label, ".continue");

      return jump_instr;
    }
    case WHILE_STMT: {
      return while_to_TAC(func_name, stmt->statement.while_stmt.condition,
                          stmt->statement.while_stmt.statement,
                          stmt->statement.while_stmt.label);
    }
    case DO_WHILE_STMT: { 
      return do_while_to_TAC(func_name, stmt->statement.do_while_stmt.statement,
                             stmt->statement.do_while_stmt.condition,
                             stmt->statement.do_while_stmt.label);

    }
    case FOR_STMT: {
      return for_to_TAC(func_name,
                        stmt->statement.for_stmt.init,
                        stmt->statement.for_stmt.condition,
                        stmt->statement.for_stmt.end,
                        stmt->statement.for_stmt.statement,
                        stmt->statement.for_stmt.label,
                        stmt->statement.for_stmt.init_idents);
    }
    case SWITCH_STMT: {
      // AST:
      // switch (cond) { cases... }
      // TAC:
      // <cond>
      // <case dispatch>
      // <body>
      // Label label.break
      struct Val* dst = (struct Val*)arena_alloc(sizeof(struct Val));
      struct TACInstr* expr_instrs = expr_to_TAC_convert(func_name, stmt->statement.switch_stmt.condition, dst);

      struct TACInstr* cases_instrs = cases_to_TAC(stmt->statement.switch_stmt.label,
                                                   stmt->statement.switch_stmt.cases,
                                                   dst);
      struct TACInstr* stmt_instrs = stmt_to_TAC(func_name, stmt->statement.switch_stmt.statement);

      struct TACInstr* break_label_instr = tac_instr_create(TACLABEL);
      break_label_instr->instr.tac_label.label = slice_concat(stmt->statement.switch_stmt.label, ".break");

      concat_TAC_instrs(&expr_instrs, cases_instrs);
      concat_TAC_instrs(&expr_instrs, stmt_instrs);
      concat_TAC_instrs(&expr_instrs, break_label_instr);

      return expr_instrs;
    }
    case CASE_STMT: {
      // AST:
      // case value: stmt
      // TAC:
      // Label case_label
      // <stmt>
      struct TACInstr* stmt_instrs = stmt_to_TAC(func_name, stmt->statement.case_stmt.statement);

      struct TACInstr* label_instr = tac_instr_create(TACLABEL);
      label_instr->instr.tac_label.label = stmt->statement.case_stmt.label;

      concat_TAC_instrs(&label_instr, stmt_instrs);
      return label_instr;
    }
    case DEFAULT_STMT: {
      // AST:
      // default: stmt
      // TAC:
      // Label default_label
      // <stmt>
      struct TACInstr* stmt_instrs = stmt_to_TAC(func_name, stmt->statement.default_stmt.statement);

      struct TACInstr* label_instr = tac_instr_create(TACLABEL);
      label_instr->instr.tac_label.label = stmt->statement.default_stmt.label;

      concat_TAC_instrs(&label_instr, stmt_instrs);
      return label_instr;
    }
    case NULL_STMT: {
      return NULL;
    }
    default:
      tac_error_at0(stmt->loc, "statement type not implemented yet");
      return NULL;
  }
}

// Purpose: Emit TAC comparisons and jumps for a switch case list.
// Inputs: label is the switch label; cases is the collected CaseList; rslt is the switch value.
// Outputs: Returns a TAC list that dispatches to case/default or break.
// Invariants/Assumptions: Case labels are unique; default is optional.
struct TACInstr* cases_to_TAC(struct Slice* label, struct CaseList* cases, struct Val* rslt) {
  struct TACInstr* case_instrs = NULL;
  struct Slice* default_label = NULL;

  for (struct CaseList* case_item = cases; case_item != NULL; case_item = case_item->next) {
    switch (case_item->case_label.type) {
      case INT_CASE: {
        // AST:
        // case <>:
        // TAC:
        // Cmp switch_val, // CondJump CondE case_label
        struct TACInstr* cmp_instr = tac_instr_create(TACCMP);
        cmp_instr->instr.tac_cmp.src1 = rslt;
        cmp_instr->instr.tac_cmp.src2 =
            tac_make_const((uint64_t)case_item->case_label.data, rslt->type);

        struct TACInstr* cond_jump_instr = tac_instr_create(TACCOND_JUMP);
        cond_jump_instr->instr.tac_cond_jump.condition = CondE;
        cond_jump_instr->instr.tac_cond_jump.label = make_case_label(label, case_item->case_label.data);

        concat_TAC_instrs(&case_instrs, cmp_instr);
        concat_TAC_instrs(&case_instrs, cond_jump_instr);
        break;
      }
      case DEFAULT_CASE:
        default_label = slice_concat(label, ".default");
        break;
      default:
        tac_error_at0(NULL, "invalid case label type");
        return NULL;
    }
  }

  struct Slice* fallthrough_label = default_label;
  if (fallthrough_label == NULL) {
    fallthrough_label = slice_concat(label, ".break");
  }

  // AST:
  // (no case matched)
  // TAC:
  // Jump default_label or break_label
  struct TACInstr* jump_instr = tac_instr_create(TACJUMP);
  jump_instr->instr.tac_jump.label = fallthrough_label;
  concat_TAC_instrs(&case_instrs, jump_instr);

  return case_instrs;
}

// Purpose: Lower a for-loop initializer into TAC instructions.
// Inputs: func_name is the owning function; init_ may be a declaration or expression.
// Outputs: Returns a TAC list for the initializer, or NULL if empty.
// Invariants/Assumptions: The initializer is already typechecked.
struct TACInstr* for_init_to_TAC(struct Slice* func_name, struct ForInit* init_) {
  if (init_ == NULL) {
    return NULL;
  }

  switch (init_->type) {
    case DCLR_INIT: {
      if (init_->init.dclr_init == NULL) {
        tac_error_at0(NULL, "for-init declaration is missing");
        return NULL;
      }
      struct Declaration tmp;
      tmp.type = VAR_DCLR;
      tmp.dclr.var_dclr = *init_->init.dclr_init;
      return var_dclr_to_TAC(func_name, &tmp);
    }
    case EXPR_INIT: {
      if (init_->init.expr_init == NULL) {
        return NULL;
      }
      struct ExprResult init_result;
      return expr_to_TAC(func_name, init_->init.expr_init, &init_result);
    }
    default:
      tac_error_at0(NULL, "invalid for-init type");
      return NULL;
  }
}

// Purpose: Lower a while loop into TAC control-flow instructions.
// Inputs: func_name is the owning function; condition/body are loop components.
// Outputs: Returns a TAC list implementing the loop.
// Invariants/Assumptions: label must be non-NULL after loop labeling.
struct TACInstr* while_to_TAC(struct Slice* func_name,
                                     struct Expr* condition,
                                     struct Statement* body,
                                     struct Slice* label) {
  if (label == NULL) {
    tac_error_at0(condition ? condition->loc : NULL, "while loop label missing");
    return NULL;
  }

  struct Slice* continue_label = slice_concat(label, ".continue");
  struct Slice* break_label = slice_concat(label, ".break");

  struct TACInstr* body_instrs = stmt_to_TAC(func_name, body);
  struct Val* cond_val = (struct Val*)arena_alloc(sizeof(struct Val));
  struct TACInstr* cond_instrs = expr_to_TAC_convert(func_name, condition, cond_val);

  struct TACInstr* instrs = NULL;

  // AST:
  // while (cond) { body }
  // TAC:
  // Label continue
  // <cond>
  // Cmp cond, 0
  // CondJump CondE break
  // <body>
  // Jump continue
  // Label break
  struct TACInstr* continue_label_instr = tac_instr_create(TACLABEL);
  continue_label_instr->instr.tac_label.label = continue_label;
  concat_TAC_instrs(&instrs, continue_label_instr);

  concat_TAC_instrs(&instrs, cond_instrs);

  struct TACInstr* cmp_instr = tac_instr_create(TACCMP);
  cmp_instr->instr.tac_cmp.src1 = cond_val;
  cmp_instr->instr.tac_cmp.src2 = tac_make_const(0, cond_val->type);

  struct TACInstr* cond_jump_instr = tac_instr_create(TACCOND_JUMP);
  cond_jump_instr->instr.tac_cond_jump.condition = CondE;
  cond_jump_instr->instr.tac_cond_jump.label = break_label;

  concat_TAC_instrs(&instrs, cmp_instr);
  concat_TAC_instrs(&instrs, cond_jump_instr);
  concat_TAC_instrs(&instrs, body_instrs);

  struct TACInstr* jump_back = tac_instr_create(TACJUMP);
  jump_back->instr.tac_jump.label = continue_label;
  concat_TAC_instrs(&instrs, jump_back);

  struct TACInstr* break_label_instr = tac_instr_create(TACLABEL);
  break_label_instr->instr.tac_label.label = break_label;
  concat_TAC_instrs(&instrs, break_label_instr);

  return instrs;
}

// Purpose: Lower a do-while loop into TAC control-flow instructions.
// Inputs: func_name is the owning function; condition/body are loop components.
// Outputs: Returns a TAC list implementing the loop.
// Invariants/Assumptions: label must be non-NULL after loop labeling.
struct TACInstr* do_while_to_TAC(struct Slice* func_name,
                                        struct Statement* body,
                                        struct Expr* condition,
                                        struct Slice* label) {
  if (label == NULL) {
    tac_error_at0(condition ? condition->loc : NULL, "do-while loop label missing");
    return NULL;
  }

  struct Slice* start_label = slice_concat(label, ".start");
  struct Slice* continue_label = slice_concat(label, ".continue");
  struct Slice* break_label = slice_concat(label, ".break");

  struct TACInstr* body_instrs = stmt_to_TAC(func_name, body);
  struct Val* cond_val = (struct Val*)arena_alloc(sizeof(struct Val));
  struct TACInstr* cond_instrs = expr_to_TAC_convert(func_name, condition, cond_val);

  struct TACInstr* instrs = NULL;

  // AST:
  // do { body } while (cond)
  // TAC:
  // Label start
  // <body>
  // Label continue
  // <cond>
  // Cmp cond, 0
  // CondJump CondNE start
  // Label break
  struct TACInstr* start_label_instr = tac_instr_create(TACLABEL);
  start_label_instr->instr.tac_label.label = start_label;
  concat_TAC_instrs(&instrs, start_label_instr);

  concat_TAC_instrs(&instrs, body_instrs);

  struct TACInstr* continue_label_instr = tac_instr_create(TACLABEL);
  continue_label_instr->instr.tac_label.label = continue_label;
  concat_TAC_instrs(&instrs, continue_label_instr);

  concat_TAC_instrs(&instrs, cond_instrs);

  struct TACInstr* cmp_instr = tac_instr_create(TACCMP);
  cmp_instr->instr.tac_cmp.src1 = cond_val;
  cmp_instr->instr.tac_cmp.src2 = tac_make_const(0, cond_val->type);

  struct TACInstr* cond_jump_instr = tac_instr_create(TACCOND_JUMP);
  cond_jump_instr->instr.tac_cond_jump.condition = CondNE;
  cond_jump_instr->instr.tac_cond_jump.label = start_label;

  concat_TAC_instrs(&instrs, cmp_instr);
  concat_TAC_instrs(&instrs, cond_jump_instr);

  struct TACInstr* break_label_instr = tac_instr_create(TACLABEL);
  break_label_instr->instr.tac_label.label = break_label;
  concat_TAC_instrs(&instrs, break_label_instr);

  return instrs;
}

// Purpose: Lower a for loop into TAC control-flow instructions.
// Inputs: func_name is the owning function; init/condition/end/body form the loop.
// Outputs: Returns a TAC list implementing the loop.
// Invariants/Assumptions: label must be non-NULL after loop labeling.
struct TACInstr* for_to_TAC(struct Slice* func_name,
                                   struct ForInit* init_,
                                   struct Expr* condition,
                                   struct Expr* end,
                                   struct Statement* body,
                                   struct Slice* label,
                                   struct IdentMap* idents) {
  if (label == NULL) {
    tac_error_at0(condition ? condition->loc : NULL, "for loop label missing");
    return NULL;
  }

  struct Slice* start_label = slice_concat(label, ".start");
  struct Slice* continue_label = slice_concat(label, ".continue");
  struct Slice* break_label = slice_concat(label, ".break");

  struct TACInstr* init_instrs = for_init_to_TAC(func_name, init_);
  struct TACInstr* body_instrs = stmt_to_TAC(func_name, body);

  struct TACInstr* condition_instrs = NULL;
  if (condition != NULL) {
    struct Val* cond_val = (struct Val*)arena_alloc(sizeof(struct Val));
    condition_instrs = expr_to_TAC_convert(func_name, condition, cond_val);

    struct TACInstr* cmp_instr = tac_instr_create(TACCMP);
    cmp_instr->instr.tac_cmp.src1 = cond_val;
    cmp_instr->instr.tac_cmp.src2 = tac_make_const(0, cond_val->type);

    struct TACInstr* cond_jump_instr = tac_instr_create(TACCOND_JUMP);
    cond_jump_instr->instr.tac_cond_jump.condition = CondE;
    cond_jump_instr->instr.tac_cond_jump.label = break_label;

    concat_TAC_instrs(&condition_instrs, cmp_instr);
    concat_TAC_instrs(&condition_instrs, cond_jump_instr);
  }

  struct TACInstr* end_instrs = NULL;
  if (end != NULL) {
    struct ExprResult end_result;
    end_instrs = expr_to_TAC(func_name, end, &end_result);
  }

  struct TACInstr* instrs = NULL;
  concat_TAC_instrs(&instrs, init_instrs);

  // AST:
  // for (init; cond; end) { body }
  // TAC:
  // <init>
  // Label start
  // <cond>
  // Cmp cond, 0
  // CondJump CondE break
  // <body>
  // Label continue
  // <end>
  // Jump start
  // Label break
  struct TACInstr* start_label_instr = tac_instr_create(TACLABEL);
  start_label_instr->instr.tac_label.label = start_label;
  concat_TAC_instrs(&instrs, start_label_instr);

  concat_TAC_instrs(&instrs, condition_instrs);
  concat_TAC_instrs(&instrs, body_instrs);

  struct TACInstr* continue_label_instr = tac_instr_create(TACLABEL);
  continue_label_instr->instr.tac_label.label = continue_label;
  concat_TAC_instrs(&instrs, continue_label_instr);

  concat_TAC_instrs(&instrs, end_instrs);

  struct TACInstr* jump_back = tac_instr_create(TACJUMP);
  jump_back->instr.tac_jump.label = start_label;
  concat_TAC_instrs(&instrs, jump_back);

  struct TACInstr* break_label_instr = tac_instr_create(TACLABEL);
  break_label_instr->instr.tac_label.label = break_label;
  concat_TAC_instrs(&instrs, break_label_instr);

  // call all cleanup functions for variables going out of scope here
  if (idents == NULL) {
    return instrs;
  }
  for (size_t i = 0; i < idents->size; ++i){
    for (struct IdentMapEntry* ident_entry = idents->arr[i]; ident_entry != NULL; ident_entry = ident_entry->next) {
      struct SymbolEntry* symbol_entry = symbol_table_get(global_symbol_table, ident_entry->entry_name);
      if (symbol_entry == NULL || symbol_entry->attrs == NULL) {
        continue;
      }
      if (symbol_entry->attrs->attr_type == LOCAL_ATTR &&
          symbol_entry->attrs->cleanup_handler != NULL) {
        struct TACInstr* cleanup_instr = tac_instr_create(TACCALL);
        cleanup_instr->instr.tac_call.func_name = symbol_entry->attrs->cleanup_handler;
        struct Val* var = tac_make_var(ident_entry->entry_name, symbol_entry->type);
        // pass in address of var
        struct Val* addr = make_temp(func_name, tac_builtin_type(UINT_TYPE));
        struct TACInstr* addr_instr = tac_instr_create(TACGET_ADDRESS);
        addr_instr->instr.tac_get_address.dst = addr;
        addr_instr->instr.tac_get_address.src = var;
        concat_TAC_instrs(&instrs, addr_instr);
        cleanup_instr->instr.tac_call.args = addr;
        cleanup_instr->instr.tac_call.dst = NULL;
        cleanup_instr->instr.tac_call.num_args = 1;
        concat_TAC_instrs(&instrs, cleanup_instr);
      }
    }
  }
  if (idents != NULL){
    destroy_ident_map(idents);
  }

  return instrs;
}

// Purpose: Lower an if statement without an else into TAC control flow.
// Inputs: func_name is the owning function; condition/if_stmt are the branches.
// Outputs: Returns a TAC list implementing the conditional.
// Invariants/Assumptions: condition is typechecked to an arithmetic value.
struct TACInstr* if_to_TAC(struct Slice* func_name, struct Expr* condition, struct Statement* if_stmt) {
  struct Val* cond_val = (struct Val*)arena_alloc(sizeof(struct Val));
  struct TACInstr* cond_instrs = expr_to_TAC_convert(func_name, condition, cond_val);
  struct TACInstr* body_instrs = stmt_to_TAC(func_name, if_stmt);

  struct Slice* end_label = tac_make_label(func_name, "end");

  // AST:
  // if (cond) { body }
  // TAC:
  // <cond>
  // Cmp cond, 0
  // CondJump CondE end
  // <body>
  // Label end
  struct TACInstr* cmp_instr = tac_instr_create(TACCMP);
  cmp_instr->instr.tac_cmp.src1 = cond_val;
  cmp_instr->instr.tac_cmp.src2 = tac_make_const(0, cond_val->type);

  struct TACInstr* cond_jump_instr = tac_instr_create(TACCOND_JUMP);
  cond_jump_instr->instr.tac_cond_jump.condition = CondE;
  cond_jump_instr->instr.tac_cond_jump.label = end_label;

  struct TACInstr* end_label_instr = tac_instr_create(TACLABEL);
  end_label_instr->instr.tac_label.label = end_label;

  struct TACInstr* instrs = NULL;
  concat_TAC_instrs(&instrs, cond_instrs);
  concat_TAC_instrs(&instrs, cmp_instr);
  concat_TAC_instrs(&instrs, cond_jump_instr);
  concat_TAC_instrs(&instrs, body_instrs);
  concat_TAC_instrs(&instrs, end_label_instr);

  return instrs;
}

// Purpose: Lower an if/else statement into TAC control flow.
// Inputs: func_name is the owning function; condition/if_stmt/else_stmt define branches.
// Outputs: Returns a TAC list implementing the conditional.
// Invariants/Assumptions: condition is typechecked to an arithmetic value.
struct TACInstr* if_else_to_TAC(struct Slice* func_name,
                                struct Expr* condition,
                                struct Statement* if_stmt,
                                struct Statement* else_stmt) {
  struct Val* cond_val = (struct Val*)arena_alloc(sizeof(struct Val));
  struct TACInstr* cond_instrs = expr_to_TAC_convert(func_name, condition, cond_val);
  struct TACInstr* if_instrs = stmt_to_TAC(func_name, if_stmt);
  struct TACInstr* else_instrs = stmt_to_TAC(func_name, else_stmt);
  struct Slice* else_label = tac_make_label(func_name, "else");
  struct Slice* end_label = tac_make_label(func_name, "end");

  // AST:
  // if (cond) { if_body } else { else_body }
  // TAC:
  // <cond>
  // Cmp cond, 0
  // CondJump CondE else
  // <if_body>
  // Jump end
  // Label else
  // <else_body>
  // Label end
  struct TACInstr* cmp_instr = tac_instr_create(TACCMP);
  cmp_instr->instr.tac_cmp.src1 = cond_val;
  cmp_instr->instr.tac_cmp.src2 = tac_make_const(0, cond_val->type);

  struct TACInstr* cond_jump_instr = tac_instr_create(TACCOND_JUMP);
  cond_jump_instr->instr.tac_cond_jump.condition = CondE;
  cond_jump_instr->instr.tac_cond_jump.label = else_label;

  struct TACInstr* jump_end_instr = tac_instr_create(TACJUMP);
  jump_end_instr->instr.tac_jump.label = end_label;

  struct TACInstr* else_label_instr = tac_instr_create(TACLABEL);
  else_label_instr->instr.tac_label.label = else_label;

  struct TACInstr* end_label_instr = tac_instr_create(TACLABEL);
  end_label_instr->instr.tac_label.label = end_label;

  struct TACInstr* instrs = NULL;
  concat_TAC_instrs(&instrs, cond_instrs);
  concat_TAC_instrs(&instrs, cmp_instr);
  concat_TAC_instrs(&instrs, cond_jump_instr);
  concat_TAC_instrs(&instrs, if_instrs);
  concat_TAC_instrs(&instrs, jump_end_instr);
  concat_TAC_instrs(&instrs, else_label_instr);
  concat_TAC_instrs(&instrs, else_instrs);
  concat_TAC_instrs(&instrs, end_label_instr);

  return instrs;
}

// Purpose: Lower a function call argument list into TAC and collect argument values.
// Inputs: func_name is the owning function; args is the linked list of expressions.
// Outputs: Returns TAC instructions for argument evaluation and fills out_args/count.
// Invariants/Assumptions: Arguments are evaluated left-to-right.
struct TACInstr* args_to_TAC(struct Slice* func_name,
                                    struct ArgList* args,
                                    struct Val** out_args,
                                    size_t* out_count) {
  size_t count = 0;
  for (struct ArgList* cur = args; cur != NULL; cur = cur->next) {
    count++;
  }

  *out_count = count;
  if (count == 0) {
    *out_args = NULL;
    return NULL;
  }

  struct Val* vals = (struct Val*)arena_alloc(sizeof(struct Val) * count);
  struct TACInstr* instrs = NULL;

  size_t idx = 0;
  for (struct ArgList* cur = args; cur != NULL; cur = cur->next) {
    struct TACInstr* arg_instrs = expr_to_TAC_convert(func_name, cur->arg, &vals[idx]);
    concat_TAC_instrs(&instrs, arg_instrs);
    idx++;
  }

  *out_args = vals;
  return instrs;
}

// Purpose: Lower a relational expression into TAC that yields a boolean int.
// Inputs: func_name is the owning function; expr/op/left/right describe the comparison.
// Outputs: Returns a TAC list and sets result to the boolean value.
// Invariants/Assumptions: Operand types are already typechecked for comparison.
struct TACInstr* relational_to_TAC(struct Slice* func_name,
                                          struct Expr* expr,
                                          enum BinOp op,
                                          struct Expr* left,
                                          struct Expr* right,
                                          struct ExprResult* result) {
  struct Val* left_val = (struct Val*)arena_alloc(sizeof(struct Val));
  struct Val* right_val = (struct Val*)arena_alloc(sizeof(struct Val));
  struct TACInstr* left_instrs = expr_to_TAC_convert(func_name, left, left_val);
  struct TACInstr* right_instrs = expr_to_TAC_convert(func_name, right, right_val);

  struct Val* dst = make_temp(func_name, expr->value_type);
  struct Slice* end_label = tac_make_label(func_name, "end");

  struct TACInstr* instrs = NULL;

  // AST:
  // (left op right) relational
  // TAC:
  // Copy dst, 1
  // <left>
  // <right>
  // Cmp left, right
  // CondJump <cond> end
  // Copy dst, 0
  // Label end
  struct TACInstr* init_copy = tac_instr_create(TACCOPY);
  init_copy->instr.tac_copy.dst = dst;
  init_copy->instr.tac_copy.src = tac_make_const(1, tac_builtin_type(INT_TYPE));
  concat_TAC_instrs(&instrs, init_copy);

  concat_TAC_instrs(&instrs, left_instrs);
  concat_TAC_instrs(&instrs, right_instrs);

  struct TACInstr* cmp_instr = tac_instr_create(TACCMP);
  cmp_instr->instr.tac_cmp.src1 = left_val;
  cmp_instr->instr.tac_cmp.src2 = right_val;

  struct TACInstr* cond_jump_instr = tac_instr_create(TACCOND_JUMP);
  cond_jump_instr->instr.tac_cond_jump.condition = relation_to_cond(op, left->value_type);
  cond_jump_instr->instr.tac_cond_jump.label = end_label;

  struct TACInstr* clear_copy = tac_instr_create(TACCOPY);
  clear_copy->instr.tac_copy.dst = dst;
  clear_copy->instr.tac_copy.src = tac_make_const(0, tac_builtin_type(INT_TYPE));

  struct TACInstr* end_label_instr = tac_instr_create(TACLABEL);
  end_label_instr->instr.tac_label.label = end_label;

  concat_TAC_instrs(&instrs, cmp_instr);
  concat_TAC_instrs(&instrs, cond_jump_instr);
  concat_TAC_instrs(&instrs, clear_copy);
  concat_TAC_instrs(&instrs, end_label_instr);

  result->type = PLAIN_OPERAND;
  result->val = dst;

  return instrs;
}

// Purpose: Lower an expression and ensure the result is a plain operand.
// Inputs: func_name is the owning function; expr is the expression to lower.
// Outputs: Returns TAC instructions; out_val receives the computed value if provided.
// Invariants/Assumptions: Loads are emitted when an lvalue is dereferenced.
struct TACInstr* expr_to_TAC_convert(struct Slice* func_name, struct Expr* expr, struct Val* dst) {
  struct ExprResult raw_result;
  struct TACInstr* instrs = expr_to_TAC(func_name, expr, &raw_result);

  switch (raw_result.type) {
    case PLAIN_OPERAND: {
      // AST:
      // expression used as value
      // TAC:
      // (no-op)
      if (dst != NULL) {
        tac_copy_val(dst, raw_result.val);
      }
      return instrs;
    }
    case DEREFERENCED_POINTER: {
      struct Val* tmp = make_temp(func_name, expr->value_type);
      tac_copy_val(dst, tmp);
    
      // AST:
      // lvalue expression used as value
      // TAC:
      // Load dst, [ptr]
      struct TACInstr* load_instr = tac_instr_create(TACLOAD);
      load_instr->instr.tac_load.dst = dst;
      load_instr->instr.tac_load.src_ptr = raw_result.val;
      concat_TAC_instrs(&instrs, load_instr);
    
      return instrs;
    }
    case SUB_OBJECT: {
      struct Val* tmp = make_temp(func_name, expr->value_type);
      tac_copy_val(dst, tmp);
      // AST:
      // struct.field or ptr->field used as value
      // TAC:
      // CopyFromOffset dst, base_ptr, offset

      struct TACInstr* copy_instr = tac_instr_create(TACCOPY_FROM_OFFSET);
      copy_instr->instr.tac_copy_from_offset.dst = dst;
      copy_instr->instr.tac_copy_from_offset.src = raw_result.sub_object_base;
      copy_instr->instr.tac_copy_from_offset.offset = raw_result.sub_object_offset;
      concat_TAC_instrs(&instrs, copy_instr);

      return instrs;
    }
    default:
      tac_error_at0(expr ? expr->loc : NULL, "unsupported ExprResult type in expr_to_TAC_convert");
      return NULL;
  }
}

// Purpose: Lower a compound-assignment binary expression into TAC instructions.
// Inputs: func_name is the owning function; expr/bin_expr/op describe the compound operation.
// Outputs: Returns TAC instruction list; result describes the computed value.
// Invariants/Assumptions: op satisfies is_compound_op(op) and types are already validated.
static struct TACInstr* compound_binary_expr_to_TAC(struct Slice* func_name,
                                                    struct Expr* expr,
                                                    struct BinaryExpr* bin_expr,
                                                    enum BinOp op,
                                                    struct ExprResult* result) {
  struct ExprResult lhs_result;
  struct TACInstr* lhs_instrs = expr_to_TAC(func_name, bin_expr->left, &lhs_result);

  struct Val* rhs_val = (struct Val*)arena_alloc(sizeof(struct Val));
  struct TACInstr* rhs_instrs = expr_to_TAC_convert(func_name, bin_expr->right, rhs_val);

  struct TACInstr* instrs = NULL;
  concat_TAC_instrs(&instrs, lhs_instrs);
  concat_TAC_instrs(&instrs, rhs_instrs);

  enum BinOp base_op = compound_to_binop(op);
  struct Type* lhs_type = bin_expr->left->value_type;
  struct Type* rhs_type = bin_expr->right->value_type;
  bool pointer_lhs = is_pointer_type(lhs_type);
  struct Type* op_type = lhs_type;
  if (!pointer_lhs && is_arithmetic_type(lhs_type) && is_arithmetic_type(rhs_type)) {
    if (base_op == BIT_SHL || base_op == BIT_SHR) {
      op_type = lhs_type;
    } else {
      op_type = get_common_type(lhs_type, rhs_type);
      if (op_type == NULL) {
        op_type = lhs_type;
      }
    }
  }

  if (lhs_result.type == PLAIN_OPERAND) {
    // AST:
    // lhs <op>= rhs (plain lvalue)
    // TAC:
    // <lhs lvalue>
    // <rhs>
    // [optional] Binary Mul scaled = rhs * sizeof(T)
    // Binary op lhs, lhs, rhs_or_scaled
    struct Val* rhs_for_op = rhs_val;
    if (pointer_lhs && (base_op == ADD_OP || base_op == SUB_OP) && is_arithmetic_type(rhs_type)) {
      struct Type* ref_type = lhs_type->type_data.pointer_type.referenced_type;
      int scale = (int)get_type_size(ref_type);
      struct Val* scaled = make_temp(func_name, rhs_type);

      // Scale integer offsets by element size for pointer arithmetic.
      struct TACInstr* mul_instr = tac_instr_create(TACBINARY);
      mul_instr->instr.tac_binary.alu_op = binop_to_aluop(MUL_OP, rhs_type);
      mul_instr->instr.tac_binary.dst = scaled;
      mul_instr->instr.tac_binary.src1 = rhs_val;
      mul_instr->instr.tac_binary.src2 =
          tac_make_const((uint64_t)scale, rhs_type);
      concat_TAC_instrs(&instrs, mul_instr);
      rhs_for_op = scaled;
    }

    bool needs_unsigned_div = !pointer_lhs &&
        (base_op == DIV_OP || base_op == MOD_OP) &&
        is_signed_type(op_type) &&
        (get_type_size(op_type) > get_type_size(lhs_type)) &&
        !is_signed_type(lhs_type);
    if (needs_unsigned_div) {
      // Signed long division/modulo with a narrower unsigned lhs must zero-extend before op.
      // The backend is 32-bit, so emulate by using unsigned ops and fixing the sign.
      struct TACInstr* div_instrs =
          emit_unsigned_lhs_signed_divmod(func_name, lhs_result.val, lhs_result.val,
                                          rhs_for_op, rhs_for_op->type,
                                          base_op == MOD_OP);
      concat_TAC_instrs(&instrs, div_instrs);
      result->type = PLAIN_OPERAND;
      result->val = lhs_result.val;
      return instrs;
    }

    struct TACInstr* bin_instr = tac_instr_create(TACBINARY);
    bin_instr->instr.tac_binary.alu_op = binop_to_aluop(base_op, op_type);
    bin_instr->instr.tac_binary.dst = lhs_result.val;
    bin_instr->instr.tac_binary.src1 = lhs_result.val;
    bin_instr->instr.tac_binary.src2 = rhs_for_op;
    concat_TAC_instrs(&instrs, bin_instr);

    result->type = PLAIN_OPERAND;
    result->val = lhs_result.val;
    return instrs;
  }

  if (lhs_result.type == DEREFERENCED_POINTER) {
    struct Val* cur = make_temp(func_name, expr->value_type);
    // AST:
    // *ptr <op>= rhs
    // TAC:
    // Load cur, [ptr]
    // [optional] Binary Mul scaled = rhs * sizeof(T)
    // Binary op cur, cur, rhs_or_scaled
    // Store [ptr], cur
    // Load lvalue before applying the compound operation.
    struct TACInstr* load_instr = tac_instr_create(TACLOAD);
    load_instr->instr.tac_load.dst = cur;
    load_instr->instr.tac_load.src_ptr = lhs_result.val;
    concat_TAC_instrs(&instrs, load_instr);

    struct Val* rhs_for_op = rhs_val;
    if (pointer_lhs && (base_op == ADD_OP || base_op == SUB_OP) && is_arithmetic_type(rhs_type)) {
      struct Type* ref_type = lhs_type->type_data.pointer_type.referenced_type;
      int scale = (int)get_type_size(ref_type);
      struct Val* scaled = make_temp(func_name, rhs_type);

      struct TACInstr* mul_instr = tac_instr_create(TACBINARY);
      mul_instr->instr.tac_binary.alu_op = binop_to_aluop(MUL_OP, rhs_type);
      mul_instr->instr.tac_binary.dst = scaled;
      mul_instr->instr.tac_binary.src1 = rhs_val;
      mul_instr->instr.tac_binary.src2 =
          tac_make_const((uint64_t)scale, rhs_type);
      concat_TAC_instrs(&instrs, mul_instr);
      rhs_for_op = scaled;
    }

    bool needs_unsigned_div = !pointer_lhs &&
        (base_op == DIV_OP || base_op == MOD_OP) &&
        is_signed_type(op_type) &&
        (get_type_size(op_type) > get_type_size(lhs_type)) &&
        !is_signed_type(lhs_type);
    if (needs_unsigned_div) {
      // Use the same unsigned-division emulation before storing back through the pointer.
      struct TACInstr* div_instrs =
          emit_unsigned_lhs_signed_divmod(func_name, cur, cur, rhs_for_op,
                                          rhs_for_op->type, base_op == MOD_OP);
      concat_TAC_instrs(&instrs, div_instrs);

      struct TACInstr* store_instr = tac_instr_create(TACSTORE);
      store_instr->instr.tac_store.dst_ptr = lhs_result.val;
      store_instr->instr.tac_store.src = cur;
      concat_TAC_instrs(&instrs, store_instr);

      result->type = PLAIN_OPERAND;
      result->val = cur;
      return instrs;
    }

    struct TACInstr* bin_instr = tac_instr_create(TACBINARY);
    bin_instr->instr.tac_binary.alu_op = binop_to_aluop(base_op, op_type);
    bin_instr->instr.tac_binary.dst = cur;
    bin_instr->instr.tac_binary.src1 = cur;
    bin_instr->instr.tac_binary.src2 = rhs_for_op;
    concat_TAC_instrs(&instrs, bin_instr);

    struct TACInstr* store_instr = tac_instr_create(TACSTORE);
    store_instr->instr.tac_store.dst_ptr = lhs_result.val;
    store_instr->instr.tac_store.src = cur;
    concat_TAC_instrs(&instrs, store_instr);

    result->type = PLAIN_OPERAND;
    result->val = cur;
    return instrs;
  }

  tac_error_at0(expr->loc, "unsupported compound assignment lvalue");
  return NULL;
}

// Purpose: Lower a binary expression into TAC instructions and an ExprResult.
// Inputs: func_name is the owning function; expr is a BINARY expression.
// Outputs: Returns TAC instruction list; result describes the computed value.
// Invariants/Assumptions: Expression types are already validated by typechecking.
static struct TACInstr* binary_expr_to_TAC(struct Slice* func_name,
                                           struct Expr* expr,
                                           struct ExprResult* result) {
  struct BinaryExpr* bin_expr = &expr->expr.bin_expr;
  enum BinOp op = bin_expr->op;

  if (op == BOOL_AND || op == BOOL_OR) {
    struct Val* left_val = (struct Val*)arena_alloc(sizeof(struct Val));
    struct Val* right_val = (struct Val*)arena_alloc(sizeof(struct Val));
    struct TACInstr* left_instrs = expr_to_TAC_convert(func_name, bin_expr->left, left_val);
    struct TACInstr* right_instrs = expr_to_TAC_convert(func_name, bin_expr->right, right_val);

    struct Val* dst = make_temp(func_name, expr->value_type);
    struct Slice* end_label = tac_make_label(func_name, "end");

    struct TACInstr* instrs = NULL;

    // AST:
    // left && right   OR   left || right
    // TAC:
    // Copy dst, <short-circuit default>
    // <left>
    // Cmp left, 0
    // CondJump <cond> end
    // <right>
    // Cmp right, 0
    // CondJump <cond> end
    // Copy dst, <final>
    // Label end
    // Default result matches the short-circuit outcome before evaluating RHS.
    struct TACInstr* init_copy = tac_instr_create(TACCOPY);
    init_copy->instr.tac_copy.dst = dst;
    init_copy->instr.tac_copy.src =
        tac_make_const(op != BOOL_AND, tac_builtin_type(INT_TYPE));
    concat_TAC_instrs(&instrs, init_copy);
    concat_TAC_instrs(&instrs, left_instrs);

    struct TACInstr* cmp_left = tac_instr_create(TACCMP);
    cmp_left->instr.tac_cmp.src1 = left_val;
    cmp_left->instr.tac_cmp.src2 = tac_make_const(0, left_val->type);

    // If the left side decides the result, skip RHS evaluation.
    struct TACInstr* jump_left = tac_instr_create(TACCOND_JUMP);
    jump_left->instr.tac_cond_jump.condition = (op == BOOL_AND) ? CondE : CondNE;
    jump_left->instr.tac_cond_jump.label = end_label;

    concat_TAC_instrs(&instrs, cmp_left);
    concat_TAC_instrs(&instrs, jump_left);
    concat_TAC_instrs(&instrs, right_instrs);

    struct TACInstr* cmp_right = tac_instr_create(TACCMP);
    cmp_right->instr.tac_cmp.src1 = right_val;
    cmp_right->instr.tac_cmp.src2 = tac_make_const(0, right_val->type);

    struct TACInstr* jump_right = tac_instr_create(TACCOND_JUMP);
    jump_right->instr.tac_cond_jump.condition = (op == BOOL_AND) ? CondE : CondNE;
    jump_right->instr.tac_cond_jump.label = end_label;

    concat_TAC_instrs(&instrs, cmp_right);
    concat_TAC_instrs(&instrs, jump_right);

    struct TACInstr* final_copy = tac_instr_create(TACCOPY);
    final_copy->instr.tac_copy.dst = dst;
    final_copy->instr.tac_copy.src =
        tac_make_const(op == BOOL_AND, tac_builtin_type(INT_TYPE));

    struct TACInstr* end_label_instr = tac_instr_create(TACLABEL);
    end_label_instr->instr.tac_label.label = end_label;

    concat_TAC_instrs(&instrs, final_copy);
    concat_TAC_instrs(&instrs, end_label_instr);

    result->type = PLAIN_OPERAND;
    result->val = dst;
    return instrs;
  }

  if (is_relational_op(op)) {
    return relational_to_TAC(func_name, expr, op, bin_expr->left, bin_expr->right, result);
  }

  if (is_compound_op(op)) {
    return compound_binary_expr_to_TAC(func_name, expr, bin_expr, op, result);
  }

  if (op == ADD_OP || op == SUB_OP) {
    struct Type* left_type = bin_expr->left->value_type;
    struct Type* right_type = bin_expr->right->value_type;

    bool left_ptr = is_pointer_type(left_type);
    bool right_ptr = is_pointer_type(right_type);

    struct Val* left_val = (struct Val*)arena_alloc(sizeof(struct Val));
    struct Val* right_val = (struct Val*)arena_alloc(sizeof(struct Val));
    struct TACInstr* left_instrs = expr_to_TAC_convert(func_name, bin_expr->left, left_val);
    struct TACInstr* right_instrs = expr_to_TAC_convert(func_name, bin_expr->right, right_val);

    struct TACInstr* instrs = NULL;
    concat_TAC_instrs(&instrs, left_instrs);
    concat_TAC_instrs(&instrs, right_instrs);

    if (!left_ptr && !right_ptr) {
      // AST:
      // left +/- right (non-pointer)
      // TAC:
      // <left>
      // <right>
      // Binary op dst, left, right
      struct Val* dst = make_temp(func_name, expr->value_type);
      struct TACInstr* bin_instr = tac_instr_create(TACBINARY);
      bin_instr->instr.tac_binary.alu_op = binop_to_aluop(op, expr->value_type);
      bin_instr->instr.tac_binary.dst = dst;
      bin_instr->instr.tac_binary.src1 = left_val;
      bin_instr->instr.tac_binary.src2 = right_val;
      concat_TAC_instrs(&instrs, bin_instr);
      result->type = PLAIN_OPERAND;
      result->val = dst;
      return instrs;
    }

    if (op == ADD_OP && left_ptr && is_arithmetic_type(right_type)) {
      // AST:
      // ptr + int
      // TAC:
      // <ptr>
      // <int>
      // Binary Mul scaled = int * sizeof(T)
      // Binary Add dst, ptr, scaled
      struct Type* ref_type = left_type->type_data.pointer_type.referenced_type;
      int scale = (int)get_type_size(ref_type);
      struct Val* scaled = make_temp(func_name, right_type);

      // Pointer +/- integer uses scaled byte offset.
      struct TACInstr* mul_instr = tac_instr_create(TACBINARY);
      mul_instr->instr.tac_binary.alu_op = binop_to_aluop(MUL_OP, right_type);
      mul_instr->instr.tac_binary.dst = scaled;
      mul_instr->instr.tac_binary.src1 = right_val;
      mul_instr->instr.tac_binary.src2 =
          tac_make_const((uint64_t)scale, right_type);
      concat_TAC_instrs(&instrs, mul_instr);

      struct Val* dst = make_temp(func_name, expr->value_type);
      struct TACInstr* add_instr = tac_instr_create(TACBINARY);
      add_instr->instr.tac_binary.alu_op = binop_to_aluop(ADD_OP, expr->value_type);
      add_instr->instr.tac_binary.dst = dst;
      add_instr->instr.tac_binary.src1 = left_val;
      add_instr->instr.tac_binary.src2 = scaled;
      concat_TAC_instrs(&instrs, add_instr);

      result->type = PLAIN_OPERAND;
      result->val = dst;
      return instrs;
    }

    if (op == ADD_OP && right_ptr && is_arithmetic_type(left_type)) {
      // AST:
      // int + ptr
      // TAC:
      // <int>
      // <ptr>
      // Binary Mul scaled = int * sizeof(T)
      // Binary Add dst, scaled, ptr
      struct Type* ref_type = right_type->type_data.pointer_type.referenced_type;
      int scale = (int)get_type_size(ref_type);
      struct Val* scaled = make_temp(func_name, left_type);

      struct TACInstr* mul_instr = tac_instr_create(TACBINARY);
      mul_instr->instr.tac_binary.alu_op = binop_to_aluop(MUL_OP, left_type);
      mul_instr->instr.tac_binary.dst = scaled;
      mul_instr->instr.tac_binary.src1 = left_val;
      mul_instr->instr.tac_binary.src2 =
          tac_make_const((uint64_t)scale, left_type);
      concat_TAC_instrs(&instrs, mul_instr);

      struct Val* dst = make_temp(func_name, expr->value_type);
      struct TACInstr* add_instr = tac_instr_create(TACBINARY);
      add_instr->instr.tac_binary.alu_op = binop_to_aluop(ADD_OP, expr->value_type);
      add_instr->instr.tac_binary.dst = dst;
      add_instr->instr.tac_binary.src1 = scaled;
      add_instr->instr.tac_binary.src2 = right_val;
      concat_TAC_instrs(&instrs, add_instr);

      result->type = PLAIN_OPERAND;
      result->val = dst;
      return instrs;
    }

    if (op == SUB_OP && left_ptr && is_arithmetic_type(right_type)) {
      // AST:
      // ptr - int
      // TAC:
      // <ptr>
      // <int>
      // Binary Mul scaled = int * sizeof(T)
      // Binary Sub dst, ptr, scaled
      struct Type* ref_type = left_type->type_data.pointer_type.referenced_type;
      int scale = (int)get_type_size(ref_type);
      struct Val* scaled = make_temp(func_name, right_type);

      struct TACInstr* mul_instr = tac_instr_create(TACBINARY);
      mul_instr->instr.tac_binary.alu_op = binop_to_aluop(MUL_OP, right_type);
      mul_instr->instr.tac_binary.dst = scaled;
      mul_instr->instr.tac_binary.src1 = right_val;
      mul_instr->instr.tac_binary.src2 =
          tac_make_const((uint64_t)scale, right_type);
      concat_TAC_instrs(&instrs, mul_instr);

      struct Val* dst = make_temp(func_name, expr->value_type);
      struct TACInstr* sub_instr = tac_instr_create(TACBINARY);
      sub_instr->instr.tac_binary.alu_op = binop_to_aluop(SUB_OP, expr->value_type);
      sub_instr->instr.tac_binary.dst = dst;
      sub_instr->instr.tac_binary.src1 = left_val;
      sub_instr->instr.tac_binary.src2 = scaled;
      concat_TAC_instrs(&instrs, sub_instr);

      result->type = PLAIN_OPERAND;
      result->val = dst;
      return instrs;
    }

    tac_error_at0(expr->loc, "invalid pointer arithmetic in binary operation");
    return NULL;
  }

  {
    struct Val* left_val = (struct Val*)arena_alloc(sizeof(struct Val));
    struct Val* right_val = (struct Val*)arena_alloc(sizeof(struct Val));
    struct TACInstr* left_instrs = expr_to_TAC_convert(func_name, bin_expr->left, left_val);
    struct TACInstr* right_instrs = expr_to_TAC_convert(func_name, bin_expr->right, right_val);

    struct Val* dst = make_temp(func_name, expr->value_type);
    // AST:
    // left <op> right
    // TAC:
    // <left>
    // <right>
    // Binary op dst, left, right
    struct TACInstr* bin_instr = tac_instr_create(TACBINARY);
    bin_instr->instr.tac_binary.alu_op = binop_to_aluop(op, expr->value_type);
    bin_instr->instr.tac_binary.dst = dst;
    bin_instr->instr.tac_binary.src1 = left_val;
    bin_instr->instr.tac_binary.src2 = right_val;

    struct TACInstr* instrs = NULL;
    concat_TAC_instrs(&instrs, left_instrs);
    concat_TAC_instrs(&instrs, right_instrs);
    concat_TAC_instrs(&instrs, bin_instr);

    result->type = PLAIN_OPERAND;
    result->val = dst;
    return instrs;
  }
}

// Purpose: Lower a function-call expression into TAC instructions and an ExprResult.
// Inputs: func_name is the owning function; expr is a FUNCTION_CALL expression.
// Outputs: Returns TAC instruction list; result describes the computed value.
// Invariants/Assumptions: Argument and callee types are already validated by typechecking.
static struct TACInstr* function_call_expr_to_TAC(struct Slice* func_name,
                                                  struct Expr* expr,
                                                  struct ExprResult* result) {
  struct Val* args = NULL;
  size_t num_args = 0;
  struct TACInstr* arg_instrs = args_to_TAC(func_name, expr->expr.fun_call_expr.args, &args, &num_args);

  struct Val* dst = NULL;
  if (expr->value_type->type != VOID_TYPE) {
    dst = make_temp(func_name, expr->value_type);
  }

  struct Expr* func_expr = expr->expr.fun_call_expr.func;
  struct Type* func_expr_type = func_expr->value_type;
  bool is_direct_call = false;
  if (func_expr->type == VAR) {
    struct SymbolEntry* entry = symbol_table_get(global_symbol_table,
                                                 func_expr->expr.var_expr.name);
    if (entry != NULL) {
      func_expr_type = entry->type;
      func_expr->value_type = entry->type;
      if (entry->type->type == FUN_TYPE) {
        is_direct_call = true;
      }
    }
  }
  if (is_direct_call) {
    // normal call

    // AST:
    // func(arg0, arg1, ...)
    // TAC:
    // <arg0>, <arg1>, ...
    // Call func -> dst
    struct TACInstr* call_instr = tac_instr_create(TACCALL);
    call_instr->instr.tac_call.func_name = func_expr->expr.var_expr.name;
    call_instr->instr.tac_call.dst = dst;
    call_instr->instr.tac_call.args = args;
    call_instr->instr.tac_call.num_args = num_args;

    struct TACInstr* instrs = NULL;
    concat_TAC_instrs(&instrs, arg_instrs);
    concat_TAC_instrs(&instrs, call_instr);

    result->type = PLAIN_OPERAND;
    result->val = dst;
    return instrs;
  }

  // indirect call
  if (func_expr_type == NULL) {
    tac_error_at0(expr->loc, "indirect call missing callee type");
    return NULL;
  }

  // AST:
  // func(arg0, arg1, ...)
  // TAC:
  // tmp <- <func>
  // <arg0>, <arg1>, ...
  // Call tmp -> dst
  struct Val* tmp = make_temp(func_name, func_expr_type);
  struct TACInstr* func_instrs = expr_to_TAC_convert(func_name, func_expr, tmp);

  struct TACInstr* call_instr = tac_instr_create(TACCALL_INDIRECT);
  call_instr->instr.tac_call_indirect.func = tmp;
  call_instr->instr.tac_call_indirect.dst = dst;
  call_instr->instr.tac_call_indirect.args = args;
  call_instr->instr.tac_call_indirect.num_args = num_args;

  struct TACInstr* instrs = NULL;
  concat_TAC_instrs(&instrs, func_instrs);
  concat_TAC_instrs(&instrs, arg_instrs);
  concat_TAC_instrs(&instrs, call_instr);

  result->type = PLAIN_OPERAND;
  result->val = dst;
  return instrs;
}

// Purpose: Lower an address-of expression into TAC instructions and an ExprResult.
// Inputs: func_name is the owning function; expr is an ADDR_OF expression.
// Outputs: Returns TAC instruction list; result describes the computed value.
// Invariants/Assumptions: The operand is typechecked before lowering.
static struct TACInstr* addr_of_expr_to_TAC(struct Slice* func_name,
                                            struct Expr* expr,
                                            struct ExprResult* result) {
  struct AddrOfExpr* addr_expr = &expr->expr.addr_of_expr;
  struct ExprResult inner_result;
  struct TACInstr* instrs = expr_to_TAC(func_name, addr_expr->expr, &inner_result);

  if (inner_result.type == PLAIN_OPERAND) {
    struct Val* dst = make_temp(func_name, expr->value_type);
    // AST:
    // &lvalue
    // TAC:
    // <lvalue>
    // GetAddress dst, lvalue
    struct TACInstr* addr_instr = tac_instr_create(TACGET_ADDRESS);
    addr_instr->instr.tac_get_address.dst = dst;
    addr_instr->instr.tac_get_address.src = inner_result.val;
    concat_TAC_instrs(&instrs, addr_instr);

    result->type = PLAIN_OPERAND;
    result->val = dst;
    return instrs;
  }

  if (inner_result.type == DEREFERENCED_POINTER) {
    // &(*p) collapses to p, so reuse the pointer operand directly.
    result->type = PLAIN_OPERAND;
    result->val = inner_result.val;
    return instrs;
  }

  if (inner_result.type == SUB_OBJECT) {
    struct Val* dst = make_temp(func_name, expr->value_type);
    // AST:
    // &struct.field  OR  &ptr->field
    // TAC:
    // <base_ptr>
    // GetAddress dst, base_ptr
    // Binary Add dst, dst, offset
    struct TACInstr* addr_instr = tac_instr_create(TACGET_ADDRESS);
    addr_instr->instr.tac_get_address.dst = dst;
    addr_instr->instr.tac_get_address.src = tac_make_var(inner_result.sub_object_base,
      tac_builtin_type(UINT_TYPE));

    struct TACInstr* offset_instr = tac_instr_create(TACBINARY);
    offset_instr->instr.tac_binary.alu_op = ALU_ADD;
    offset_instr->instr.tac_binary.dst = dst;
    offset_instr->instr.tac_binary.src1 = dst;
    offset_instr->instr.tac_binary.src2 = tac_make_const((uint64_t)inner_result.sub_object_offset,
      tac_builtin_type(UINT_TYPE));

    concat_TAC_instrs(&instrs, addr_instr);
    concat_TAC_instrs(&instrs, offset_instr);

    result->type = PLAIN_OPERAND;
    result->val = dst;
    return instrs;
  }

  tac_error_at0(expr->loc, "invalid address-of operand");
  return NULL;
}

// Purpose: Lower a subscript expression into TAC instructions and an ExprResult.
// Inputs: func_name is the owning function; expr is a SUBSCRIPT expression.
// Outputs: Returns TAC instruction list; result describes the computed lvalue pointer.
// Invariants/Assumptions: The base expression has already typechecked as pointer-like.
static struct TACInstr* subscript_expr_to_TAC(struct Slice* func_name,
                                              struct Expr* expr,
                                              struct ExprResult* result) {
  struct SubscriptExpr* sub_expr = &expr->expr.subscript_expr;
  struct Val* base_ptr_val = (struct Val*)arena_alloc(sizeof(struct Val));
  struct TACInstr* base_ptr_instrs = expr_to_TAC_convert(func_name, sub_expr->array, base_ptr_val);

  struct Val* index_val = (struct Val*)arena_alloc(sizeof(struct Val));
  struct TACInstr* index_instrs = expr_to_TAC_convert(func_name, sub_expr->index, index_val);

  struct TACInstr* instrs = NULL;
  concat_TAC_instrs(&instrs, base_ptr_instrs);
  concat_TAC_instrs(&instrs, index_instrs);

  // AST:
  // base_ptr[index]
  // TAC:
  // <base_ptr>
  // <index>
  // Binary Mul offset = index * sizeof(T)
  // Binary Add addr = base_ptr + offset
  struct Type* ref_type = expr->value_type;
  size_t scale = get_type_size(ref_type);
  struct Val* offset = make_temp(func_name, index_val->type);

  struct TACInstr* mul_instr = tac_instr_create(TACBINARY);
  mul_instr->instr.tac_binary.alu_op = binop_to_aluop(MUL_OP, index_val->type);
  mul_instr->instr.tac_binary.dst = offset;
  mul_instr->instr.tac_binary.src1 = index_val;
  mul_instr->instr.tac_binary.src2 = tac_make_const((uint64_t)scale, index_val->type);
  concat_TAC_instrs(&instrs, mul_instr);

  struct Val* addr = make_temp(func_name, tac_builtin_type(UINT_TYPE));
  struct TACInstr* add_instr = tac_instr_create(TACBINARY);
  add_instr->instr.tac_binary.alu_op = binop_to_aluop(ADD_OP, addr->type);
  add_instr->instr.tac_binary.dst = addr;
  add_instr->instr.tac_binary.src1 = base_ptr_val;
  add_instr->instr.tac_binary.src2 = offset;
  concat_TAC_instrs(&instrs, add_instr);

  result->type = DEREFERENCED_POINTER;
  result->val = addr;
  return instrs;
}

// Purpose: Lower a statement expression into TAC instructions and an ExprResult.
// Inputs: func_name is the owning function; expr is a STMT_EXPR expression.
// Outputs: Returns TAC instruction list; result describes the final value, if any.
// Invariants/Assumptions: The statement-expression block is non-empty and typechecked.
static struct TACInstr* stmt_expr_to_TAC(struct Slice* func_name,
                                         struct Expr* expr,
                                         struct ExprResult* result) {
  struct StmtExpr* stmt_expr = &expr->expr.stmt_expr;
  if (stmt_expr->block == NULL) {
    tac_error_at0(expr->loc, "empty statement expression in TAC lowering");
    return NULL;
  }

  struct Block* last_item = stmt_expr->block;
  while (last_item->next != NULL) {
    last_item = last_item->next;
  }

  struct TACInstr* instrs = NULL;

  for (struct Block* cur = stmt_expr->block; cur != NULL; cur = cur->next) {
    // Loop through each item here instead of doing it recursively so the last
    // expression statement can supply the statement-expression value directly.
    struct TACInstr* item_instrs = NULL;
    bool is_last = (cur == last_item);

    switch (cur->item->type) {
      case STMT_ITEM: {
        struct Statement* stmt = cur->item->item.stmt;
        if (is_last && stmt->type == EXPR_STMT) {
          // Last expression statement provides the statement-expression value.
          struct Expr* tail_expr = stmt->statement.expr_stmt.expr;
          if (expr->value_type->type == VOID_TYPE) {
            item_instrs = expr_to_TAC_convert(func_name, tail_expr, NULL);
            result->type = PLAIN_OPERAND;
            result->val = NULL;
          } else {
            struct Val* dst = make_temp(func_name, expr->value_type);
            item_instrs = expr_to_TAC_convert(func_name, tail_expr, dst);
            result->type = PLAIN_OPERAND;
            result->val = dst;
          }
        } else {
          item_instrs = stmt_to_TAC(func_name, stmt);
          if (is_last) {
            result->type = PLAIN_OPERAND;
            result->val = NULL;
          }
        }

        if (debug_info_enabled) {
          struct TACInstr* boundary_before = tac_instr_create(TACBOUNDARY);
          boundary_before->instr.tac_boundary.loc = stmt->loc;
          concat_TAC_instrs(&boundary_before, item_instrs);
          item_instrs = boundary_before;
        }
        break;
      }
      case DCLR_ITEM: {
        item_instrs = local_dclr_to_TAC(func_name, cur->item->item.dclr);
        if (debug_info_enabled) {
          char* loc = declaration_loc(cur->item->item.dclr);
          if (loc != NULL) {
            struct TACInstr* boundary_dclr = tac_instr_create(TACBOUNDARY);
            boundary_dclr->instr.tac_boundary.loc = loc;
            concat_TAC_instrs(&boundary_dclr, item_instrs);
            item_instrs = boundary_dclr;
          }
        }
        if (is_last) {
          result->type = PLAIN_OPERAND;
          result->val = NULL;
        }
        break;
      }
      default:
        tac_error_at0(expr->loc, "invalid block item type in statement expression");
        return NULL;
    }

    concat_TAC_instrs(&instrs, item_instrs);
  }

  return instrs;
}

// Purpose: Lower a direct struct/union member access into TAC and an ExprResult.
// Inputs: func_name is the owning function; expr is a DOT_EXPR expression.
// Outputs: Returns TAC instructions for the base expression and field address math.
// Invariants/Assumptions: The base expression is already typechecked as a struct/union lvalue.
static struct TACInstr* dot_expr_to_TAC(struct Slice* func_name,
                                        struct Expr* expr,
                                        struct ExprResult* result) {
  struct DotExpr* dot_expr = &expr->expr.dot_expr;
  struct ExprResult* base_result = (struct ExprResult*)arena_alloc(sizeof(struct ExprResult));
  struct TACInstr* base_instrs = expr_to_TAC(func_name, dot_expr->struct_expr, base_result);

  struct Type* base_type = dot_expr->struct_expr->value_type;
  struct MemberEntry* field_entry = get_struct_member(base_type, dot_expr->member);
  if (field_entry == NULL) {
    tac_error_at1_slice(expr->loc, "struct/union has no field named '%.*s'", dot_expr->member);
    return NULL;
  }

  size_t field_offset = field_entry->offset;
  struct TACInstr* instrs = base_instrs;

  switch (base_result->type) {
    case PLAIN_OPERAND:
      // AST:
      // base.field
      // TAC:
      // <base>
      if (base_result->val->val_type != VARIABLE) {
        tac_error_at0(expr->loc, "invalid base expression for dot operator");
        return NULL;
      }
      result->type = SUB_OBJECT;
      result->sub_object_base = base_result->val->val.var_name;
      result->sub_object_offset = field_offset;
      return instrs;
    case DEREFERENCED_POINTER: {
      struct Val* dst_ptr = make_temp(func_name, tac_builtin_type(UINT_TYPE));

      // AST:
      // (base_ptr)->field
      // TAC:
      // <base_ptr>
      // Binary Add dst_ptr = base_ptr + offset
      struct TACInstr* add_instr = tac_instr_create(TACBINARY);
      add_instr->instr.tac_binary.alu_op = binop_to_aluop(ADD_OP, dst_ptr->type);
      add_instr->instr.tac_binary.dst = dst_ptr;
      add_instr->instr.tac_binary.src1 = base_result->val;
      add_instr->instr.tac_binary.src2 =
          tac_make_const((uint64_t)field_offset, tac_builtin_type(UINT_TYPE));
      concat_TAC_instrs(&instrs, add_instr);

      result->type = DEREFERENCED_POINTER;
      result->val = dst_ptr;
      return instrs;
    }
    case SUB_OBJECT:
      // AST:
      // base.sub_object.field
      // TAC:
      // <base.field>
      result->type = SUB_OBJECT;
      result->sub_object_base = base_result->sub_object_base;
      result->sub_object_offset = base_result->sub_object_offset + field_offset;
      return instrs;
    default:
      tac_error_at0(expr->loc, "invalid base expression for dot operator");
      return NULL;
  }
}

// Purpose: Lower a pointer member access into TAC and an ExprResult.
// Inputs: func_name is the owning function; expr is an ARROW_EXPR expression.
// Outputs: Returns TAC instructions for the base pointer and computed field address.
// Invariants/Assumptions: The operand typechecks as pointer-to-struct/union before lowering.
static struct TACInstr* arrow_expr_to_TAC(struct Slice* func_name,
                                          struct Expr* expr,
                                          struct ExprResult* result) {
  struct ArrowExpr* arrow_expr = &expr->expr.arrow_expr;
  struct Val* base_ptr_val = (struct Val*)arena_alloc(sizeof(struct Val));
  struct TACInstr* base_ptr_instrs =
      expr_to_TAC_convert(func_name, arrow_expr->pointer_expr, base_ptr_val);
  struct Type* base_type = arrow_expr->pointer_expr->value_type;
  if (base_type->type != POINTER_TYPE) {
    tac_error_at0(expr->loc, "arrow operator requires pointer to struct/union");
    return NULL;
  }
  base_type = base_type->type_data.pointer_type.referenced_type;

  struct MemberEntry* field_entry = get_struct_member(base_type, arrow_expr->member);
  if (field_entry == NULL) {
    tac_error_at1_slice(expr->loc, "struct/union has no field named '%.*s'", arrow_expr->member);
    return NULL;
  }

  size_t field_offset = field_entry->offset;
  struct TACInstr* instrs = base_ptr_instrs;
  struct Val* dst_ptr = make_temp(func_name, tac_builtin_type(UINT_TYPE));

  // AST:
  // base_ptr->field
  // TAC:
  // <base_ptr>
  // Binary Add dst_ptr = base_ptr + offset
  struct TACInstr* add_instr = tac_instr_create(TACBINARY);
  add_instr->instr.tac_binary.alu_op = binop_to_aluop(ADD_OP, dst_ptr->type);
  add_instr->instr.tac_binary.dst = dst_ptr;
  add_instr->instr.tac_binary.src1 = base_ptr_val;
  add_instr->instr.tac_binary.src2 =
      tac_make_const((uint64_t)field_offset, tac_builtin_type(UINT_TYPE));
  concat_TAC_instrs(&instrs, add_instr);

  result->type = DEREFERENCED_POINTER;
  result->val = dst_ptr;
  return instrs;
}

// Purpose: Lower an expression into TAC instructions and an ExprResult.
// Inputs: func_name is the owning function; expr is the expression to lower.
// Outputs: Returns TAC instruction list; result describes the computed value.
// Invariants/Assumptions: Expression types are already validated by typechecking.
struct TACInstr* expr_to_TAC(struct Slice* func_name, struct Expr* expr, struct ExprResult* result) {
  if (result == NULL) {
    tac_error_at0(expr ? expr->loc : NULL, "expr_to_TAC requires a result output");
    return NULL;
  }

  switch (expr->type) {
    case BINARY:
      return binary_expr_to_TAC(func_name, expr, result);
    case ASSIGN: {
      struct AssignExpr* assign_expr = &expr->expr.assign_expr;
      struct ExprResult lhs_result;
      struct TACInstr* lhs_instrs = expr_to_TAC(func_name, assign_expr->left, &lhs_result);

      struct Val* rhs_val = (struct Val*)arena_alloc(sizeof(struct Val));
      struct TACInstr* rhs_instrs = expr_to_TAC_convert(func_name, assign_expr->right, rhs_val);

      struct TACInstr* instrs = NULL;
      concat_TAC_instrs(&instrs, lhs_instrs);
      concat_TAC_instrs(&instrs, rhs_instrs);

      if (lhs_result.type == PLAIN_OPERAND) {
        // AST:
        // lhs = rhs (plain lvalue)
        // TAC:
        // <lhs lvalue>
        // <rhs>
        // Copy lhs, rhs
        struct TACInstr* copy_instr = tac_instr_create(TACCOPY);
        copy_instr->instr.tac_copy.dst = lhs_result.val;
        copy_instr->instr.tac_copy.src = rhs_val;
        concat_TAC_instrs(&instrs, copy_instr);

        result->type = PLAIN_OPERAND;
        result->val = lhs_result.val;
        return instrs;
      }

      if (lhs_result.type == DEREFERENCED_POINTER) {
        // AST:
        // *ptr = rhs
        // TAC:
        // <ptr>
        // <rhs>
        // Store [ptr], rhs
        struct TACInstr* store_instr = tac_instr_create(TACSTORE);
        store_instr->instr.tac_store.dst_ptr = lhs_result.val;
        store_instr->instr.tac_store.src = rhs_val;
        concat_TAC_instrs(&instrs, store_instr);

        result->type = PLAIN_OPERAND;
        result->val = rhs_val;
        return instrs;
      }

      if (lhs_result.type == SUB_OBJECT) {
        // AST:
        // struct.field = rhs  OR  ptr->field = rhs
        // TAC:
        // <base_ptr>
        // <rhs>
        // CopyToOffset base_ptr, offset, rhs
        struct TACInstr* copy_instr = tac_instr_create(TACCOPY_TO_OFFSET);
        copy_instr->instr.tac_copy_to_offset.dst = lhs_result.sub_object_base;
        copy_instr->instr.tac_copy_to_offset.offset = lhs_result.sub_object_offset;
        copy_instr->instr.tac_copy_to_offset.src = rhs_val;
        copy_instr->instr.tac_copy_to_offset.dst_type = assign_expr->left->value_type;
        concat_TAC_instrs(&instrs, copy_instr);

        result->type = PLAIN_OPERAND;
        result->val = rhs_val;
        return instrs;
      }

      tac_error_at0(expr->loc, "invalid assignment target");
      return NULL;
    }
    case POST_ASSIGN: {
      struct PostAssignExpr* post_assign = &expr->expr.post_assign_expr;
      if (post_assign->expr == NULL) {
        tac_error_at0(expr->loc, "post-assignment requires an lvalue");
        return NULL;
      }

      struct ExprResult lhs_result;
      struct TACInstr* lhs_instrs = expr_to_TAC(func_name, post_assign->expr, &lhs_result);

      enum BinOp bin_op = (post_assign->op == POST_INC) ? ADD_OP : SUB_OP;
      struct Val* step_val = tac_make_const(1, tac_builtin_type(INT_TYPE));
      if (is_pointer_type(expr->value_type)) {
        struct Type* ref_type = expr->value_type->type_data.pointer_type.referenced_type;
        step_val = tac_make_const((uint64_t)get_type_size(ref_type),
                                  tac_builtin_type(INT_TYPE));
      }

      struct Val* old_val = make_temp(func_name, expr->value_type);
      struct TACInstr* instrs = NULL;
      concat_TAC_instrs(&instrs, lhs_instrs);

      if (lhs_result.type == PLAIN_OPERAND) {
        struct Val* src = lhs_result.val;

        // AST:
        // x++ or x--
        // TAC:
        // Copy old, x
        // Binary op x, x, step
        struct TACInstr* copy_instr = tac_instr_create(TACCOPY);
        copy_instr->instr.tac_copy.dst = old_val;
        copy_instr->instr.tac_copy.src = src;
        concat_TAC_instrs(&instrs, copy_instr);

        struct TACInstr* bin_instr = tac_instr_create(TACBINARY);
        bin_instr->instr.tac_binary.alu_op = binop_to_aluop(bin_op, expr->value_type);
        bin_instr->instr.tac_binary.dst = src;
        bin_instr->instr.tac_binary.src1 = src;
        bin_instr->instr.tac_binary.src2 = step_val;
        concat_TAC_instrs(&instrs, bin_instr);

        result->type = PLAIN_OPERAND;
        result->val = old_val;
        return instrs;
      }

      if (lhs_result.type == DEREFERENCED_POINTER) {
        // AST:
        // (*ptr)++ or (*ptr)--
        // TAC:
        // Load old, [ptr]
        // Binary new, old, step
        // Store [ptr], new
        struct TACInstr* load_instr = tac_instr_create(TACLOAD);
        load_instr->instr.tac_load.dst = old_val;
        load_instr->instr.tac_load.src_ptr = lhs_result.val;
        concat_TAC_instrs(&instrs, load_instr);

        struct Val* new_val = make_temp(func_name, expr->value_type);
        struct TACInstr* bin_instr = tac_instr_create(TACBINARY);
        bin_instr->instr.tac_binary.alu_op = binop_to_aluop(bin_op, expr->value_type);
        bin_instr->instr.tac_binary.dst = new_val;
        bin_instr->instr.tac_binary.src1 = old_val;
        bin_instr->instr.tac_binary.src2 = step_val;
        concat_TAC_instrs(&instrs, bin_instr);

        struct TACInstr* store_instr = tac_instr_create(TACSTORE);
        store_instr->instr.tac_store.dst_ptr = lhs_result.val;
        store_instr->instr.tac_store.src = new_val;
        concat_TAC_instrs(&instrs, store_instr);

        result->type = PLAIN_OPERAND;
        result->val = old_val;
        return instrs;
      }

      tac_error_at0(expr->loc, "post-assignment requires an lvalue");
      return NULL;
    }
    case CONDITIONAL: {
      struct ConditionalExpr* cond_expr = &expr->expr.conditional_expr;

      struct TACInstr* instrs = NULL;

      struct Val* cond_val = (struct Val*)arena_alloc(sizeof(struct Val));
      struct TACInstr* cond_instrs = expr_to_TAC_convert(func_name, cond_expr->condition, cond_val);
      concat_TAC_instrs(&instrs, cond_instrs);

      struct Val* left_val = (struct Val*)arena_alloc(sizeof(struct Val));
      struct TACInstr* left_instrs = expr_to_TAC_convert(func_name, cond_expr->left, left_val);

      struct Val* right_val = (struct Val*)arena_alloc(sizeof(struct Val));
      struct TACInstr* right_instrs = expr_to_TAC_convert(func_name, cond_expr->right, right_val);

      struct Slice* else_label = tac_make_label(func_name, "else");
      struct Slice* end_label = tac_make_label(func_name, "end");
      struct Val* dst = make_temp(func_name, expr->value_type);

      // AST:
      // cond ? left : right
      // TAC:
      // <cond>
      // Cmp cond, 0
      // CondJump CondE else
      // <left>
      // Copy dst, left (if not void)
      // Jump end
      // Label else
      // <right>
      // Copy dst, right (if not void)
      // Label end

      struct TACInstr* cmp_instr = tac_instr_create(TACCMP);
      cmp_instr->instr.tac_cmp.src1 = cond_val;
      cmp_instr->instr.tac_cmp.src2 = tac_make_const(0, cond_val->type);
      concat_TAC_instrs(&instrs, cmp_instr);

      struct TACInstr* cond_jump_instr = tac_instr_create(TACCOND_JUMP);
      cond_jump_instr->instr.tac_cond_jump.condition = CondE;
      cond_jump_instr->instr.tac_cond_jump.label = else_label;
      concat_TAC_instrs(&instrs, cond_jump_instr);

      concat_TAC_instrs(&instrs, left_instrs);

      if (expr->value_type->type != VOID_TYPE){
        struct TACInstr* copy_left = tac_instr_create(TACCOPY);
        copy_left->instr.tac_copy.dst = dst;
        copy_left->instr.tac_copy.src = left_val;
        concat_TAC_instrs(&instrs, copy_left);
      }

      struct TACInstr* jump_end = tac_instr_create(TACJUMP);
      jump_end->instr.tac_jump.label = end_label;
      concat_TAC_instrs(&instrs, jump_end);

      struct TACInstr* else_label_instr = tac_instr_create(TACLABEL);
      else_label_instr->instr.tac_label.label = else_label;
      concat_TAC_instrs(&instrs, else_label_instr);

      concat_TAC_instrs(&instrs, right_instrs);

      if (expr->value_type->type != VOID_TYPE){
        struct TACInstr* copy_right = tac_instr_create(TACCOPY);
        copy_right->instr.tac_copy.dst = dst;
        copy_right->instr.tac_copy.src = right_val;
        concat_TAC_instrs(&instrs, copy_right);
      }

      struct TACInstr* end_label_instr = tac_instr_create(TACLABEL);
      end_label_instr->instr.tac_label.label = end_label;
      concat_TAC_instrs(&instrs, end_label_instr);

      result->type = PLAIN_OPERAND;
      result->val = dst;
      return instrs;
    }
    case LIT: {
      struct LitExpr* lit = &expr->expr.lit_expr;
      uint64_t const_value = 0;
      switch (lit->type) {
        case INT_CONST:
          const_value = (uint64_t)(int64_t)lit->value.int_val;
          break;
        case UINT_CONST:
          const_value = (uint64_t)lit->value.uint_val;
          break;
        case LONG_CONST:
        case ULONG_CONST:
          tac_error_at0(expr->loc,
                        "bootstrap bcc does not support long integer literals");
          return NULL;
        default:
          tac_error_at0(expr->loc, "unknown literal type in TAC lowering");
          return NULL;
      }

      result->type = PLAIN_OPERAND;
      result->val = tac_make_const(const_value, expr->value_type);
      return NULL;
    }
    case UNARY: {
      struct UnaryExpr* unary_expr = &expr->expr.un_expr;
      if (unary_expr->op == BOOL_NOT) {
        struct Val* src_val = (struct Val*)arena_alloc(sizeof(struct Val));
        struct TACInstr* src_instrs = expr_to_TAC_convert(func_name, unary_expr->expr, src_val);

        struct Val* dst = make_temp(func_name, expr->value_type);
        struct Slice* end_label = tac_make_label(func_name, "end");

        struct TACInstr* instrs = NULL;

        // AST:
        // !expr
        // TAC:
        // Copy dst, 1
        // <expr>
        // Cmp src, 0
        // CondJump CondE end
        // Copy dst, 0
        // Label end
        struct TACInstr* init_copy = tac_instr_create(TACCOPY);
        init_copy->instr.tac_copy.dst = dst;
        init_copy->instr.tac_copy.src = tac_make_const(1, tac_builtin_type(INT_TYPE));
        concat_TAC_instrs(&instrs, init_copy);
        concat_TAC_instrs(&instrs, src_instrs);

        struct TACInstr* cmp_instr = tac_instr_create(TACCMP);
        cmp_instr->instr.tac_cmp.src1 = src_val;
        cmp_instr->instr.tac_cmp.src2 = tac_make_const(0, src_val->type);

        struct TACInstr* cond_jump_instr = tac_instr_create(TACCOND_JUMP);
        cond_jump_instr->instr.tac_cond_jump.condition = CondE;
        cond_jump_instr->instr.tac_cond_jump.label = end_label;

        struct TACInstr* clear_copy = tac_instr_create(TACCOPY);
        clear_copy->instr.tac_copy.dst = dst;
        clear_copy->instr.tac_copy.src = tac_make_const(0, tac_builtin_type(INT_TYPE));

        struct TACInstr* end_label_instr = tac_instr_create(TACLABEL);
        end_label_instr->instr.tac_label.label = end_label;

        concat_TAC_instrs(&instrs, cmp_instr);
        concat_TAC_instrs(&instrs, cond_jump_instr);
        concat_TAC_instrs(&instrs, clear_copy);
        concat_TAC_instrs(&instrs, end_label_instr);

        result->type = PLAIN_OPERAND;
        result->val = dst;
        return instrs;
      }

      struct Val* src_val = (struct Val*)arena_alloc(sizeof(struct Val));
      struct TACInstr* src_instrs = expr_to_TAC_convert(func_name, unary_expr->expr, src_val);
      struct Val* dst = make_temp(func_name, expr->value_type);

      // AST:
      // op expr
      // TAC:
      // <expr>
      // Unary op dst, src
      struct TACInstr* un_instr = tac_instr_create(TACUNARY);
      un_instr->instr.tac_unary.op = unary_expr->op;
      un_instr->instr.tac_unary.dst = dst;
      un_instr->instr.tac_unary.src = src_val;

      struct TACInstr* instrs = NULL;
      concat_TAC_instrs(&instrs, src_instrs);
      concat_TAC_instrs(&instrs, un_instr);

      result->type = PLAIN_OPERAND;
      result->val = dst;
      return instrs;
    }
    case VAR: {
      result->type = PLAIN_OPERAND;
      result->val = tac_make_var(expr->expr.var_expr.name, expr->value_type);
      return NULL;
    }
    case FUNCTION_CALL:
      return function_call_expr_to_TAC(func_name, expr, result);
    case CAST: {
      struct CastExpr* cast_expr = &expr->expr.cast_expr;
      struct Val* src_val = (struct Val*)arena_alloc(sizeof(struct Val));
      struct TACInstr* src_instrs = expr_to_TAC_convert(func_name, cast_expr->expr, src_val);

      if (cast_expr->target->type == VOID_TYPE) {
        // Casting to void is a no-op.
        result->type = PLAIN_OPERAND;
        result->val = src_val;
        return src_instrs;
      }

      size_t target_size = get_type_size(cast_expr->target);
      size_t src_size = get_type_size(cast_expr->expr->value_type);
      if (target_size == 0 || src_size == 0) {
        tac_error_at2_size(expr->loc, "unsupported cast between sizes %zu and %zu", target_size, src_size);
        return NULL;
      }

      if (target_size == src_size ||
        (target_size > src_size && is_unsigned_type(cast_expr->expr->value_type))) {
        // No-op cast between same-size types, or zero-extend to larger unsigned type.
        // AST:
        // (T)expr
        // TAC:
        // <expr>
        result->type = PLAIN_OPERAND;
        result->val = src_val;
        return src_instrs;
      }

      if (target_size < src_size) {
        // Truncating cast.
        struct Val* dst = make_temp(func_name, cast_expr->target);
        // AST:
        // (T)expr
        // TAC:
        // <expr>
        // Trunc dst, src

        struct TACInstr* trunc_instr = tac_instr_create(TACTRUNC);
        trunc_instr->instr.tac_trunc.dst = dst;
        trunc_instr->instr.tac_trunc.src = src_val;
        trunc_instr->instr.tac_trunc.target_size = target_size;

        concat_TAC_instrs(&src_instrs, src_instrs);
        concat_TAC_instrs(&src_instrs, trunc_instr);

        result->type = PLAIN_OPERAND;
        result->val = dst;
        return src_instrs;
      }

      // can assume target type is signed here, so we sign extend
      if (target_size > src_size) {
        // Extending cast.
        struct Val* dst = make_temp(func_name, cast_expr->target);
        // AST:
        // (T)expr
        // TAC:
        // <expr>
        // Extend dst, src

        struct TACInstr* extend_instr = tac_instr_create(TACEXTEND);
        extend_instr->instr.tac_extend.dst = dst;
        extend_instr->instr.tac_extend.src = src_val;
        extend_instr->instr.tac_extend.src_size = src_size;
        concat_TAC_instrs(&src_instrs, src_instrs);
        concat_TAC_instrs(&src_instrs, extend_instr);

        result->type = PLAIN_OPERAND;
        result->val = dst;
        return src_instrs;
      }
    }
    case ADDR_OF:
      return addr_of_expr_to_TAC(func_name, expr, result);
    case DEREFERENCE: {
      struct DereferenceExpr* deref_expr = &expr->expr.deref_expr;
      struct Val* ptr_val = (struct Val*)arena_alloc(sizeof(struct Val));
      struct TACInstr* instrs = expr_to_TAC_convert(func_name, deref_expr->expr, ptr_val);

      result->type = DEREFERENCED_POINTER;
      result->val = ptr_val;
      return instrs;
    }
    case SUBSCRIPT:
      return subscript_expr_to_TAC(func_name, expr, result);
    case STRING: {
      struct StringExpr* str_expr = &expr->expr.string_expr;
      struct Val* str_label = make_str_label(str_expr);

      struct IdentAttr* const_attr = arena_alloc(sizeof(struct IdentAttr));
      const_attr->attr_type = CONST_ATTR;
      const_attr->is_defined = true;
      const_attr->storage = STATIC;
      const_attr->init.init_type = INITIAL;
      const_attr->init.init_list = arena_alloc(sizeof(struct InitList));
      const_attr->init.init_list->next = NULL;
      const_attr->init.init_list->value = arena_alloc(sizeof(struct StaticInit));
      const_attr->init.init_list->value->int_type = STRING_INIT;
      const_attr->init.init_list->value->value.string = str_expr->string;
      // Ensure the emitted data includes the null terminator via explicit padding.
      size_t array_size = str_expr->string->len + 1;
      size_t element_size = get_type_size(&kCharType);
      if (str_expr->string->len < array_size) {
        struct InitList* pad_node = arena_alloc(sizeof(struct InitList));
        pad_node->value = arena_alloc(sizeof(struct StaticInit));
        pad_node->value->int_type = ZERO_INIT;
        pad_node->value->value.num = (array_size - str_expr->string->len) * element_size;
        pad_node->next = NULL;
        const_attr->init.init_list->next = pad_node;
      }

      symbol_table_insert(global_symbol_table, str_label->val.var_name, 
        expr->value_type, const_attr);

      result->type = PLAIN_OPERAND;
      result->val = tac_make_var(str_label->val.var_name, expr->value_type);
      return NULL;
    }
    case SIZEOF_EXPR: {
      struct SizeOfExpr* sizeof_expr = &expr->expr.sizeof_expr;
      size_t type_size = get_type_size(sizeof_expr->expr->value_type);

      result->type = PLAIN_OPERAND;
      result->val = tac_make_const(type_size, tac_builtin_type(UINT_TYPE));
      return NULL;
    }
    case SIZEOF_T_EXPR: {
      struct SizeOfTExpr* sizeof_type_expr = &expr->expr.sizeof_t_expr;
      size_t type_size = get_type_size(sizeof_type_expr->type);

      result->type = PLAIN_OPERAND;
      result->val = tac_make_const(type_size, tac_builtin_type(UINT_TYPE));
      return NULL;
    }
    case STMT_EXPR:
      return stmt_expr_to_TAC(func_name, expr, result);
    case DOT_EXPR:
      return dot_expr_to_TAC(func_name, expr, result);
    case ARROW_EXPR:
      return arrow_expr_to_TAC(func_name, expr, result);
    default:
      tac_error_at1_int(expr->loc, "expression type %d not implemented in TAC lowering", expr->type);
      return NULL;
  }
}

// Purpose: Append a TAC instruction list onto an existing list.
// Inputs: old_instrs points to the head pointer; new_instrs is the list to append.
// Outputs: Updates *old_instrs to include new_instrs at the tail.
// Invariants/Assumptions: Both lists use the `last` pointer for O(1) concatenation.
void concat_TAC_instrs(struct TACInstr** old_instrs, struct TACInstr* new_instrs) {
  if (new_instrs == NULL) {
    return;
  }

  if (new_instrs->last == NULL) {
    new_instrs->last = tac_find_last(new_instrs);
  }

  if (*old_instrs == NULL) {
    *old_instrs = new_instrs;
    return;
  }

  if ((*old_instrs)->last == NULL) {
    (*old_instrs)->last = tac_find_last(*old_instrs);
  }

  (*old_instrs)->last->next = new_instrs;
  (*old_instrs)->last = new_instrs->last;
}
