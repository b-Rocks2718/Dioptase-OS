#ifndef CODEGEN_H
#define CODEGEN_H

#include "asm_gen.h"

struct MachineInstr;

enum MachineInstrType {
  // real instructions
  MACHINE_AND,
  MACHINE_NAND,
  MACHINE_OR,
  MACHINE_NOR,
  MACHINE_XOR,
  MACHINE_XNOR,
  MACHINE_NOT,
  MACHINE_LSL,
  MACHINE_LSR,
  MACHINE_ASR,
  MACHINE_ROTL,
  MACHINE_ROTR,
  MACHINE_LSLC,
  MACHINE_LSRC,
  MACHINE_ADD,
  MACHINE_ADDC,
  MACHINE_SUB,
  MACHINE_SUBB,
  MACHINE_EXTEND_B,
  MACHINE_EXTEND_D,
  MACHINE_TRUNCATE_B,
  MACHINE_TRUNCATE_D,
  MACHINE_LUI,
  MACHINE_SWA,
  MACHINE_LWA,
  MACHINE_LW,
  MACHINE_SW,
  MACHINE_SDA,
  MACHINE_LDA,
  MACHINE_LD,
  MACHINE_SD,
  MACHINE_SBA,
  MACHINE_LBA,
  MACHINE_LB,
  MACHINE_SB,
  MACHINE_BR,
  MACHINE_BZ,
  MACHINE_BNZ,
  MACHINE_BS,
  MACHINE_BNS,
  MACHINE_BC, 
  MACHINE_BNC,
  MACHINE_BO,
  MACHINE_BNO,
  MACHINE_BPS,
  MACHINE_BNPS,
  MACHINE_BG,
  MACHINE_BGE,
  MACHINE_BL,
  MACHINE_BLE,
  MACHINE_BA,
  MACHINE_BAE,
  MACHINE_BB,
  MACHINE_BBE,
  MACHINE_BRA,
  MACHINE_BZA,
  MACHINE_BNZA,
  MACHINE_BSA,
  MACHINE_BNSA,
  MACHINE_BCA, 
  MACHINE_BNCA,
  MACHINE_BOA,
  MACHINE_BNOA,
  MACHINE_BPSA,
  MACHINE_BNPSA,
  MACHINE_BGA,
  MACHINE_BGEA,
  MACHINE_BLA,
  MACHINE_BLEA,
  MACHINE_BAA,
  MACHINE_BAEA,
  MACHINE_BBA,
  MACHINE_BBEA,
  MACHINE_TNCB,
  MACHINE_TNCD,
  MACHINE_SXTB,
  MACHINE_SXTD,
  MACHINE_SYS,

  // macros
  MACHINE_NOP,
  MACHINE_PUSH,
  MACHINE_POP,
  MACHINE_PUSHD,
  MACHINE_POPD,
  MACHINE_PUSHB,
  MACHINE_POPB,
  MACHINE_MOV,
  MACHINE_MOVI,
  MACHINE_CALL,
  MACHINE_RET,
  MACHINE_JMP,
  MACHINE_CMP,

  // directives
  MACHINE_FILL,
  MACHINE_FILD,
  MACHINE_FILB,
  MACHINE_SPACE,
  MACHINE_GLOBAL,
  MACHINE_SECTION,
  MACHINE_ALIGN,

  // other
  MACHINE_COMMENT,
  MACHINE_NLCOMMENT,
  MACHINE_NEWLINE,
  MACHINE_LABEL,
  MACHINE_DEBUG_LOC,
  MACHINE_DEBUG_LOCAL,
};

enum Exception {
  EXC_EXIT,
};

struct MachineProg {
  struct MachineInstr* head;
  struct MachineInstr* tail;
};

struct MachineInstr {
  enum MachineInstrType type;

  enum Reg ra;
  enum Reg rb;
  enum Reg rc;

  int  imm;

  struct Slice* label;

  char* debug_loc; // source pointer for debug line markers
  struct Slice* debug_name; // debug local name for stack layout comments
  int debug_offset; // stack offset relative to BP for debug locals

  enum Exception exc;

  struct MachineInstr* next;
};

struct MachineProg* prog_to_machine(struct AsmProg* asm_prog);

struct MachineProg* top_level_to_machine(struct AsmTopLevel* asm_top);

struct MachineProg* instr_to_machine(struct Slice* name, struct AsmInstr* instr);

#endif // CODEGEN_H
