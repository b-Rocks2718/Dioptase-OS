#ifndef ASM_GEN_H
#define ASM_GEN_H

#include "AST.h"
#include "typechecking.h"
#include "TAC.h"
#include "slice.h"

#include "../crt/stdbool.h"
#include "../crt/stddef.h"

struct AsmSymbolTable;
struct AsmTopLevel;
struct AsmInstr;
struct Operand;

extern struct AsmSymbolTable* asm_symbol_table;

enum AsmTypeType {
  BYTE = 1,
  DOUBLE,
  WORD,
  LONG_WORD,
  BYTE_ARRAY,
};

struct ByteArray {
  size_t size;
  size_t alignment;
};

struct AsmType {
  enum AsmTypeType type;
  struct ByteArray byte_array;
};

struct AsmSymbolEntry{
  struct Slice* key;
  struct AsmType* type; // for data
  bool is_static;  // for data
  bool is_defined; // for functions
  bool return_on_stack; // for functions

  struct AsmSymbolEntry* next;
};

struct AsmSymbolTable{
  size_t size;
  struct AsmSymbolEntry** arr;
};

struct AsmProg {
  struct AsmTopLevel* head;
  struct AsmTopLevel* tail;
};

// Purpose: Capture a stack-local debug entry for assembly emission.
// Inputs/Outputs: name identifies the source variable; offset is relative to BP.
// Invariants/Assumptions: offset matches the lowered stack frame layout.
struct DebugLocal {
  struct Slice* name;
  int offset;
  size_t size;
  struct DebugLocal* next;
};

enum AsmTopLevelType {
  ASM_FUNC,
  ASM_STATIC_VAR,
  ASM_STATIC_CONST,
  ASM_SECTION,
  ASM_ALIGN,
};

struct AsmTopLevel {
  enum AsmTopLevelType type;
  struct Slice* name;
  bool global;

  struct AsmInstr* body; // for Func
  struct DebugLocal* locals; // for Func debug output
  size_t num_locals; // for Func debug output

  int alignment; // for StaticVar
  struct InitList* init_values; // for StaticVar

  struct AsmTopLevel* next;
};

enum AsmInstrType {
  ASM_MOV,
  ASM_UNARY,
  ASM_BINARY,
  ASM_CMP,
  ASM_PUSH,
  ASM_CALL,
  ASM_INDIRECT_CALL,
  ASM_JUMP,
  ASM_COND_JUMP,
  ASM_LABEL,
  ASM_RET,
  ASM_GET_ADDRESS,
  ASM_LOAD,
  ASM_STORE,
  ASM_BOUNDARY,
  ASM_TRUNC,
  ASM_EXTEND,
};

struct AsmInstr {
  enum AsmInstrType type;

  enum UnOp unary_op; // for Unary
  enum ALUOp alu_op;     // for Binary
  enum TACCondition cond;   // for CondJump

  size_t size; // for truncate/extend

  struct Operand* dst;
  struct Operand* src1;
  struct Operand* src2; // for binary

  struct Slice* label; // for Jump, CondJump, Label

  char* loc; // for debug line markers

  struct AsmInstr* next;
};

enum OperandType {
  OPERAND_LIT,
  OPERAND_REG,
  OPERAND_PSEUDO,
  OPERAND_PSEUDO_MEM,
  OPERAND_MEMORY,
  OPERAND_DATA
};

enum Reg {
  R0 = 0,
  R1,
  R2,
  R3,
  R4,
  R5,
  R6,
  R7,
  R8,
  R9,
  R10,
  R11,
  R12,
  R13,
  R14,
  R15,
  R16,
  R17,
  R18,
  R19,
  R20,
  R21,
  R22,
  R23,
  R24,
  R25,
  R26,
  R27,
  R28,
  R29,
  R30,
  R31
};

static enum Reg BP = R30; // base pointer register
static enum Reg SP = R31; // stack pointer register
static enum Reg RA = R29; // return address register

struct Operand {
  enum OperandType type;
  struct AsmType* asm_type;
  
  enum Reg reg;          // for Reg / Memory
  int lit_value;        // for Lit / PsuedoMem / Memory / Data
  struct Slice* pseudo;  // for Pseudo
};

struct PseudoEntry {
  struct Operand* pseudo;
  struct Operand* mapped;
  struct PseudoEntry* next;
};

struct PseudoMap{
  size_t size;
  struct PseudoEntry** arr;
};

enum VarClass {
  MEMORY_CLASS,
  INTEGER_CLASS,
};

struct VarClassList {
  enum VarClass var_class;
  struct VarClassList* next;
};

struct OperandList {
  struct Operand* opr;
  struct OperandList* next;
};

// Use caller-saved registers that are not argument registers for codegen scratch work.
extern enum Reg kScratchRegA;
extern enum Reg kScratchRegB;
extern enum Reg kScratchRegC;

// Purpose: Lower TAC into ASM, optionally emitting section directives.
// Inputs: tac_prog is the TAC program; emit_sections controls .data/.text emission.
// Outputs: Returns the ASM program or exits on internal error.
// Invariants/Assumptions: TAC top-level lists are well-formed and acyclic.
struct AsmProg* prog_to_asm(struct TACProg* tac_prog, bool emit_sections);

struct AsmTopLevel* top_level_to_asm(struct TopLevel* tac_top);

struct AsmInstr* instr_to_asm(struct Slice* func_name, struct TACInstr* tac_instr);

struct AsmInstr* set_up_params(struct Slice* func_name,
                               struct Slice** params,
                               size_t num_params,
                               bool return_in_memory);

struct Operand* tac_val_to_asm(struct Val* val);

struct Operand* make_pseudo(struct Slice* var_name, struct AsmType* asm_type);

struct Operand* make_pseudo_mem(struct Slice* var_name, struct AsmType* asm_type, int offset);

struct Operand** get_ops(struct AsmInstr* asm_instr, size_t* out_count);

struct Operand** get_srcs(struct AsmInstr* asm_instr, size_t* out_count);

struct Operand* get_dst(struct AsmInstr* asm_instr);

// Purpose: Build stack slot mappings for pseudo operands.
// Inputs: asm_instr is the function body; reserved_bytes is preallocated frame space.
// Outputs: Returns the total stack allocation in bytes (including reserved + padding).
// Invariants/Assumptions: reserved_bytes preserves ABI-mandated slots (e.g., return pointer).
size_t create_maps(struct AsmInstr* asm_instr, size_t reserved_bytes);

void replace_pseudo(struct AsmInstr* asm_instr);

size_t type_alignment(struct Type* type, struct Slice* symbol_name);

struct PseudoMap* create_pseudo_map(size_t numBuckets);

void pseudo_map_insert(struct PseudoMap* hmap, struct Operand* key, struct Operand* value);

struct Operand* pseudo_map_get(struct PseudoMap* hmap, struct Operand* key);

bool pseudo_map_contains(struct PseudoMap* hmap, struct Operand* key);

size_t asm_type_size(struct AsmType* type);

void print_pseudo_map(struct Slice* func, struct PseudoMap* hmap);

void destroy_pseudo_map(struct PseudoMap* hmap);

// Purpose: Print a debugging representation of an ASM program.
// Inputs: prog is the ASM program to print (may be NULL).
// Outputs: Writes a readable summary to stdout.
// Invariants/Assumptions: The program list is well-formed and acyclic.
void print_asm_prog(struct AsmProg* prog);

void print_asm_symbols(struct AsmSymbolTable* sym_table);

// Purpose: Detect whether a pseudo operand maps to a static storage symbol.
// Inputs: opr is the operand to classify.
// Outputs: Returns true if the operand names a static symbol.
// Invariants/Assumptions: global_symbol_table is initialized before use.
bool is_static_symbol_operand(struct Operand* opr);

// Purpose: Reserve space for a new stack slot in the current frame.
// Inputs: operand to allocate space for, and stack_bytes tracks the total allocated stack bytes.
// Outputs: Returns the negative offset from BP for the new slot.
// Invariants/Assumptions: stack_bytes is non-NULL.
int allocate_stack_slot(struct Operand* opr, size_t* stack_bytes);

// Purpose: Calculate total stack size needed for arguments passed on the stack.
// Inputs: args is the array of argument values; num_args is the number of arguments.
// Outputs: Returns the total size in bytes needed for stack arguments.
size_t get_stack_size(struct Val* args, size_t num_args);

// Purpose: Replace a pseudo operand field with its mapped location if present.
// Inputs: field points to an operand field that may hold a pseudo.
// Outputs: Updates *field in place when a mapping exists.
// Invariants/Assumptions: pseudo_map is initialized before use.
void replace_operand_if_pseudo(struct Operand** field);

// classify function parameters into register and stack arguments
void classify_params(struct Val* params, size_t num_params, bool return_in_memory,
                     struct OperandList** reg_args, struct OperandList** stack_args);

// classify function return value into operand list of reg or pseudo mem
void classify_return_val(struct Val* ret_val, struct OperandList** ret_var_list, bool* return_in_memory);

size_t asm_type_alignment(struct AsmType* type);

struct AsmSymbolTable* create_asm_symbol_table(size_t numBuckets);

void asm_symbol_table_insert(struct AsmSymbolTable* hmap, struct Slice* key, 
    struct AsmType* type, bool is_static, bool is_defined, bool return_on_stack);

struct AsmSymbolEntry* asm_symbol_table_get(struct AsmSymbolTable* hmap, struct Slice* key);

bool asm_symbol_table_contains(struct AsmSymbolTable* hmap, struct Slice* key);

void print_asm_symbol_table(struct AsmSymbolTable* hmap);

#endif // ASM_GEN_H
