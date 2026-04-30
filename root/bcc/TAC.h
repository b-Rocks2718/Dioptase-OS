#ifndef TAC_H
#define TAC_H

#include "AST.h"
#include "typechecking.h"

#include "../crt/stdint.h"

struct TopLevel;
struct TACInstr;

struct TACProg {
  struct TopLevel* head;    // Function top-levels in source order.
  struct TopLevel* tail;    // Tail of the function list for append operations.
  struct TopLevel* statics; // Static variable entries collected from symbols.
};

enum TopLevelType {
  FUNC,
  STATIC_VAR,
  STATIC_CONST,
};

struct TopLevel {
  enum TopLevelType type;
  struct Slice* name;
  bool global;

  struct TACInstr* body; // for Func
  struct Slice** params; // for Func
  size_t num_params;    // for Func
  
  struct Type* var_type; // for StaticVar and StaticConst
  struct InitList* init_values; // for StaticVar and StaticConst
  
  struct TopLevel* next;
};

enum ValType {
  CONSTANT,
  VARIABLE
};

union ValVariant {
  uint64_t const_value; // stores raw constant bits for 32/64-bit integers
  struct Slice* var_name;
};

struct Val {
  enum ValType val_type;
  union ValVariant val;
  struct Type* type;
};

enum TACInstrType {
  TACRETURN,
  TACUNARY,
  TACBINARY,
  TACCOND_JUMP,
  TACCMP,
  TACJUMP,
  TACLABEL,
  TACCOPY,
  TACCALL,
  TACCALL_INDIRECT,
  TACGET_ADDRESS,
  TACLOAD,
  TACSTORE,
  TACCOPY_TO_OFFSET,
  TACCOPY_FROM_OFFSET,
  TACBOUNDARY,
  TACTRUNC,
  TACEXTEND,
};

enum TACCondition {
  CondE,
  CondNE,
  CondG,
  CondGE,
  CondL,
  CondLE,
  CondA,
  CondAE,
  CondB,
  CondBE
};

struct TACReturn {
  struct Val* dst;
};

struct TACUnary {
  enum UnOp op;
  struct Val* dst;
  struct Val* src;
};

enum ALUOp {
  ALU_ADD,
  ALU_SUB,
  ALU_SMUL,
  ALU_SDIV,
  ALU_SMOD,
  ALU_UMUL,
  ALU_UDIV,
  ALU_UMOD,
  ALU_AND,
  ALU_OR,
  ALU_XOR,
  ALU_LSL,
  ALU_LSR,
  ALU_ASL,
  ALU_ASR,
  ALU_MOV, // ignore first arg, copy second arg to dst
};

struct TACBinary {
  enum ALUOp alu_op;
  struct Val* dst;
  struct Val* src1;
  struct Val* src2;
};

struct TACCondJump {
  enum TACCondition condition;
  struct Slice* label;
};

struct TACCmp {
  struct Val* src1;
  struct Val* src2;
};

struct TACJump {
  struct Slice* label;
};

struct TACLabel {
  struct Slice* label;
};

struct TACCopy {
  struct Val* dst;
  struct Val* src;
};

struct TACCall {
  struct Slice* func_name;
  struct Val* dst;
  struct Val* args;
  size_t num_args;
};

struct TACCallIndirect {
  struct Val* func;
  struct Val* dst;
  struct Val* args;
  size_t num_args;
};

struct TACGetAddress {
  struct Val* dst;
  struct Val* src;
};

struct TACLoad {
  struct Val* dst;
  struct Val* src_ptr;
};

struct TACStore {
  struct Val* dst_ptr;
  struct Val* src;
};

struct TACCopyToOffset {
  struct Slice* dst;
  struct Val* src;
  int offset;
  struct Type* dst_type;
};

struct TACCopyFromOffset {
  struct Val* dst;
  struct Slice* src;
  int offset;
};

struct TACBoundary {
  char* loc; // start of the statement for debug line markers
};

struct TACTrunc {
  struct Val* dst;
  struct Val* src;
  size_t target_size; // in bytes
};

struct TACExtend {
  struct Val* dst;
  struct Val* src;
  size_t src_size; // in bytes
};

union TACInstrVariant {
  struct TACReturn tac_return;
  struct TACUnary tac_unary;
  struct TACBinary tac_binary;
  struct TACCondJump tac_cond_jump;
  struct TACCmp tac_cmp;
  struct TACJump tac_jump;
  struct TACLabel tac_label;
  struct TACCopy tac_copy;
  struct TACCall tac_call;
  struct TACCallIndirect tac_call_indirect;
  struct TACGetAddress tac_get_address;
  struct TACLoad tac_load;
  struct TACStore tac_store;
  struct TACCopyToOffset tac_copy_to_offset;
  struct TACCopyFromOffset tac_copy_from_offset;
  struct TACBoundary tac_boundary; // used for statement/declaration debug line markers
  struct TACTrunc tac_trunc;
  struct TACExtend tac_extend;
};

struct TACInstr {
  enum TACInstrType type;
  union TACInstrVariant instr;
  struct TACInstr* next;
  struct TACInstr* last; // for convenience in building lists
};

enum ExprResultType {
  PLAIN_OPERAND,
  DEREFERENCED_POINTER,
  SUB_OBJECT,
};

struct ExprResult {
  enum ExprResultType type;
  struct Val* val;
  struct Slice* sub_object_base; // for SUB_OBJECT
  int sub_object_offset;          // for SUB_OBJECT
};

// ----- Main TAC conversion functions -----

// Purpose: Lower a full program into TAC, optionally emitting debug boundaries.
// Inputs: program is the typed AST; emit_debug_info controls boundary emission.
// Outputs: Returns a TAC program with top-level lists or NULL on failure.
// Invariants/Assumptions: Program declarations are in source order.
struct TACProg* prog_to_TAC(struct Program* program, bool emit_debug_info);

struct TopLevel* file_scope_dclr_to_TAC(struct Declaration* declaration);

struct TopLevel* symbol_to_TAC(struct SymbolEntry* symbol);

struct TopLevel* func_to_TAC(struct FunctionDclr* declaration);

struct TACInstr* block_to_TAC(struct Slice* func_name, struct Block* block);

struct TACInstr* local_dclr_to_TAC(struct Slice* func_name, struct Declaration* dclr);

struct TACInstr* var_dclr_to_TAC(struct Slice* func_name, struct Declaration* dclr);

struct TACInstr* stmt_to_TAC(struct Slice* func_name, struct Statement* stmt);

struct TACInstr* expr_to_TAC_convert(struct Slice* func_name, struct Expr* expr, struct Val* out_val);

struct TACInstr* expr_to_TAC(struct Slice* func_name, struct Expr* expr, struct ExprResult* result);

struct TACInstr* if_to_TAC(struct Slice* func_name, struct Expr* condition, struct Statement* if_stmt);

struct TACInstr* if_else_to_TAC(struct Slice* func_name, struct Expr* condition, struct Statement* if_stmt, struct Statement* else_stmt);

struct TACInstr* cases_to_TAC(struct Slice* label, struct CaseList* cases, struct Val* rslt);

struct TACInstr* relational_to_TAC(struct Slice* func_name,
                                          struct Expr* expr,
                                          enum BinOp op,
                                          struct Expr* left,
                                          struct Expr* right,
                                          struct ExprResult* result);

struct TACInstr* args_to_TAC(struct Slice* func_name,
                                    struct ArgList* args,
                                    struct Val** out_args,
                                    size_t* out_count);

struct TACInstr* for_init_to_TAC(struct Slice* func_name, struct ForInit* init_);

struct TACInstr* while_to_TAC(struct Slice* func_name,
                                     struct Expr* condition,
                                     struct Statement* body,
                                     struct Slice* label);

struct TACInstr* do_while_to_TAC(struct Slice* func_name,
                                        struct Statement* body,
                                        struct Expr* condition,
                                        struct Slice* label);

struct TACInstr* for_to_TAC(struct Slice* func_name,
                                   struct ForInit* init_,
                                   struct Expr* condition,
                                   struct Expr* end,
                                   struct Statement* body,
                                   struct Slice* label,
                                   struct IdentMap* idents);

// ----- Utility functions -----

void concat_TAC_instrs(struct TACInstr** old_instrs, struct TACInstr* new_instrs);

struct Val* make_temp(struct Slice* func_name, struct Type* type);

void print_static_init(struct InitList* init);

void print_tac_prog(struct TACProg* prog);

// ----- TAC interpreter -----

// Purpose: Execute a TAC program and return the integer result of main().
// Inputs: prog is the TAC program to interpret.
// Outputs: Returns the integer result produced by the main function.
// Invariants/Assumptions: main takes no parameters in this interpreter.
int tac_interpret_prog(struct TACProg* prog);

#ifdef TAC_INTERNAL
static void tac_error_at(char* loc, char* fmt, ...);

static struct TACInstr* tac_instr_create(enum TACInstrType type);

static struct TACInstr* tac_find_last(struct TACInstr* instr);

static struct Val* tac_make_const(uint64_t value, struct Type* type);

static struct Val* tac_make_var(struct Slice* name, struct Type* type);

static void tac_copy_val(struct Val* dst, struct Val* src);

static struct Slice* tac_make_label(struct Slice* func_name, char* suffix);

static bool is_relational_op(enum BinOp op);

static bool is_compound_op(enum BinOp op);

static enum BinOp compound_to_binop(enum BinOp op);

static enum TACCondition relation_to_cond(enum BinOp op, struct Type* type);
#endif

#endif // TAC_H
