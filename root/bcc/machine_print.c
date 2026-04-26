#include "machine_print.h"

#include "../crt/stdio.h"
#include "../crt/print.h"

#include "asm_gen.h"
#include "slice.h"
#include "source_location.h"

// Purpose: Recover the underlying Dioptase fd for a FILE shim stream.
// Inputs: out may be NULL.
// Outputs: Returns the underlying fd, or -1 for NULL.
// Invariants/Assumptions: stdio.h maps FILE storage to a single integer fd.
static int stream_fd(FILE* out) {
  if (out == NULL) {
    return -1;
  }
  return *out;
}

static void stream_printf1_int(FILE* out, char* fmt, int arg0) {
  int args[1];

  args[0] = arg0;
  fdprintf(stream_fd(out), fmt, args);
}

static void stream_printf1_size(FILE* out, char* fmt, size_t arg0) {
  int args[1];

  args[0] = (int)arg0;
  fdprintf(stream_fd(out), fmt, args);
}

static void stream_printf1_str(FILE* out, char* fmt, char* arg0) {
  int args[1];

  args[0] = (int)arg0;
  fdprintf(stream_fd(out), fmt, args);
}

static void stream_printf2_int(FILE* out, char* fmt, int arg0, int arg1) {
  int args[2];

  args[0] = arg0;
  args[1] = arg1;
  fdprintf(stream_fd(out), fmt, args);
}

static void stream_printf2_str_size(FILE* out, char* fmt, char* arg0, size_t arg1) {
  int args[2];

  args[0] = (int)arg0;
  args[1] = (int)arg1;
  fdprintf(stream_fd(out), fmt, args);
}

// Purpose: Write a slice to a file without assuming NUL termination.
// Inputs: out is the file to write to; slice may be NULL.
// Outputs: Emits slice text or a placeholder to the file.
// Invariants/Assumptions: out is valid and writable.
static void write_slice(FILE* out, struct Slice* slice) {
  if (slice == NULL) {
    fputs("<null>", out);
    return;
  }
  fwrite(slice->start, 1, slice->len, out);
}

// Purpose: Write a register name in assembler syntax.
// Inputs: out is the file to write to; reg is the register number.
// Outputs: Emits an assembler register name (e.g., r1).
// Invariants/Assumptions: reg is a valid enum Reg value.
static void write_reg(FILE* out, enum Reg reg) {
  switch (reg) {
    case R29:
      fputs("ra", out);
      return;
    case R30:
      fputs("bp", out);
      return;
    case R31:
      fputs("sp", out);
      return;
    default:
      stream_printf1_int(out, "r%u", (int)reg);
      return;
  }
}

// Purpose: Write a label or immediate literal for operands.
// Inputs: out is the file to write to; label may be NULL; imm is the literal.
// Outputs: Emits label text if provided, otherwise a decimal literal.
// Invariants/Assumptions: label is a valid Slice if non-NULL.
static void write_label_or_imm(FILE* out, struct Slice* label, int imm) {
  if (label != NULL) {
    write_slice(out, label);
  } else {
    stream_printf1_int(out, "%d", imm);
  }
}

// Purpose: Write a memory operand in assembler syntax.
// Inputs: out is the file to write to; base is the base register; imm is offset.
// Outputs: Emits a memory operand like [rB, imm].
// Invariants/Assumptions: base is a valid enum Reg value.
static void write_mem_operand(FILE* out, enum Reg base, int imm) {
  fputc('[', out);
  write_reg(out, base);
  stream_printf1_int(out, ", %d]", imm);
}

// Purpose: Write a memory operand that may use a label for PC-relative addressing.
// Inputs: out is the file to write to; base is the base register; label/imm select the offset.
// Outputs: Emits a memory operand like [rB, label] or [rB, imm].
// Invariants/Assumptions: label is a valid Slice if non-NULL.
static void write_mem_operand_label(FILE* out, enum Reg base, struct Slice* label, int imm) {
  fputc('[', out);
  write_reg(out, base);
  fputs(", ", out);
  write_label_or_imm(out, label, imm);
  fputc(']', out);
}

// Purpose: Emit a leading tab for non-label lines.
// Inputs: out is the file to write to.
// Outputs: Writes a single tab character.
// Invariants/Assumptions: out is valid and writable.
static void write_tab(FILE* out) {
  fputc('\t', out);
}

// Purpose: Decide whether to emit a register or immediate operand for binary ops.
// Inputs: instr is the machine instruction being printed.
// Outputs: Returns true to print instr->rc, false to print instr->imm.
// Invariants/Assumptions: instr uses rc for register operands and imm for immediates.
static bool use_reg_operand(struct MachineInstr* instr) {
  return instr->imm == 0;
}

// Purpose: Write a binary operation with either reg or immediate operand.
// Inputs: out is the file to write to; mnem is the opcode; instr is the operation.
// Outputs: Emits the formatted binary instruction.
// Invariants/Assumptions: instr->ra/rb are set and either rc or imm is meaningful.
static void write_binary_op(FILE* out,
                            char* mnem,
                            struct MachineInstr* instr) {
  stream_printf1_str(out, "%s ", mnem);
  write_reg(out, instr->ra);
  fputc(' ', out);
  write_reg(out, instr->rb);
  fputc(' ', out);
  if (use_reg_operand(instr)) {
    write_reg(out, instr->rc);
  } else {
    stream_printf1_int(out, "%d", instr->imm);
  }
  fputc('\n', out);
}

// Purpose: Emit a single machine instruction as assembly text.
// Inputs: out is the file to write to; instr is the machine instruction.
// Outputs: Returns false if an unknown instruction is encountered.
// Invariants/Assumptions: instr is non-NULL and variants are populated.
static bool write_machine_instr(FILE* out, struct MachineInstr* instr) {
  switch (instr->type) {
    case MACHINE_LABEL:
      write_slice(out, instr->label);
      fputs(":\n", out);
      return true;
    case MACHINE_DEBUG_LOC: {
      if (instr->debug_loc == NULL) {
        return true;
      }
      struct SourceLocation loc = source_location_from_ptr(instr->debug_loc);
      char* filename = source_filename_for_ptr(instr->debug_loc);
      write_tab(out);
      stream_printf2_str_size(out, ".line %s %zu\n", filename, loc.line);
      return true;
    }
    case MACHINE_DEBUG_LOCAL:
      if (instr->debug_name == NULL) {
        return true;
      }
      write_tab(out);
      fputs(".local ", out);
      write_slice(out, instr->debug_name);
      stream_printf2_int(out, " %d %d\n", instr->debug_offset, instr->imm); // offset, size
      return true;
    case MACHINE_COMMENT:
      write_tab(out);
      fputs("# ", out);
      write_slice(out, instr->label);
      fputc('\n', out);
      return true;
    case MACHINE_NLCOMMENT:
      fputc('\n', out);
      write_tab(out);
      fputs("# ", out);
      write_slice(out, instr->label);
      fputc('\n', out);
      return true;
    case MACHINE_NEWLINE:
      fputc('\n', out);
      return true;
    case MACHINE_GLOBAL:
      write_tab(out);
      fputs(".global ", out);
      write_slice(out, instr->label);
      fputc('\n', out);
      return true;
    case MACHINE_SECTION:
      write_tab(out);
      fputs("\n\t.", out);
      write_slice(out, instr->label);
      fputc('\n', out);
      return true;
    case MACHINE_FILL:
      write_tab(out);
      fputs(".fill ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_FILD:
      write_tab(out);
      stream_printf1_int(out, ".fild %d\n", instr->imm);
      return true;
    case MACHINE_FILB:
      write_tab(out);
      stream_printf1_int(out, ".filb %d\n", instr->imm);
      return true;
    case MACHINE_SPACE:
      write_tab(out);
      stream_printf1_int(out, ".space %d\n", instr->imm);
      return true;
    case MACHINE_ALIGN:
      write_tab(out);
      stream_printf1_int(out, ".align %d\n", instr->imm);
      return true;
    case MACHINE_MOV:
      write_tab(out);
      fputs("mov ", out);
      write_reg(out, instr->ra);
      fputc(' ', out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_MOVI:
      write_tab(out);
      fputs("movi ", out);
      write_reg(out, instr->ra);
      fputc(' ', out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_LUI:
      write_tab(out);
      fputs("lui ", out);
      write_reg(out, instr->ra);
      fputc(' ', out);
      stream_printf1_int(out, "%d\n", instr->imm);
      return true;
    case MACHINE_AND:
      write_tab(out);
      write_binary_op(out, "and", instr);
      return true;
    case MACHINE_NAND:
      write_tab(out);
      write_binary_op(out, "nand", instr);
      return true;
    case MACHINE_OR:
      write_tab(out);
      write_binary_op(out, "or", instr);
      return true;
    case MACHINE_NOR:
      write_tab(out);
      write_binary_op(out, "nor", instr);
      return true;
    case MACHINE_XOR:
      write_tab(out);
      write_binary_op(out, "xor", instr);
      return true;
    case MACHINE_XNOR:
      write_tab(out);
      write_binary_op(out, "xnor", instr);
      return true;
    case MACHINE_ADD:
      write_tab(out);
      write_binary_op(out, "add", instr);
      return true;
    case MACHINE_ADDC:
      write_tab(out);
      write_binary_op(out, "addc", instr);
      return true;
    case MACHINE_SUB:
      write_tab(out);
      write_binary_op(out, "sub", instr);
      return true;
    case MACHINE_SUBB:
      write_tab(out);
      write_binary_op(out, "subb", instr);
      return true;
    case MACHINE_NOT:
      write_tab(out);
      fputs("not ", out);
      write_reg(out, instr->ra);
      fputc(' ', out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_LSL:
      write_tab(out);
      fputs("lsl ", out);
      write_reg(out, instr->ra);
      fputc(' ', out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_LSR:
      write_tab(out);
      fputs("lsr ", out);
      write_reg(out, instr->ra);
      fputc(' ', out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_ASR:
      write_tab(out);
      fputs("asr ", out);
      write_reg(out, instr->ra);
      fputc(' ', out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_ROTL:
      write_tab(out);
      fputs("rotl ", out);
      write_reg(out, instr->ra);
      fputc(' ', out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_ROTR:
      write_tab(out);
      fputs("rotr ", out);
      write_reg(out, instr->ra);
      fputc(' ', out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_LSLC:
      write_tab(out);
      write_binary_op(out, "lslc", instr);
      return true;
    case MACHINE_LSRC:
      write_tab(out);
      write_binary_op(out, "lsrc", instr);
      return true;
    case MACHINE_EXTEND_B:
      write_tab(out);
      fputs("extend_b ", out);
      write_reg(out, instr->ra);
      fputc(' ', out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_EXTEND_D:
      write_tab(out);
      fputs("extend_d ", out);
      write_reg(out, instr->ra);
      fputc(' ', out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_TRUNCATE_B:
      write_tab(out);
      fputs("truncate_b ", out);
      write_reg(out, instr->ra);
      fputc(' ', out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_TRUNCATE_D:
      write_tab(out);
      fputs("truncate_d ", out);
      write_reg(out, instr->ra);
      fputc(' ', out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_SWA:
      write_tab(out);
      fputs("swa ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_mem_operand(out, instr->rb, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_LWA:
      write_tab(out);
      fputs("lwa ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_mem_operand(out, instr->rb, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_SW:
      write_tab(out);
      fputs("sw ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      if (instr->label != NULL) {
        if (instr->rb == R0 && instr->imm == 0) {
          fputc('[', out);
          write_label_or_imm(out, instr->label, instr->imm);
          fputc(']', out);
        } else {
          write_mem_operand_label(out, instr->rb, instr->label, instr->imm);
        }
      } else {
        write_mem_operand(out, instr->rb, instr->imm);
      }
      fputc('\n', out);
      return true;
    case MACHINE_LW:
      write_tab(out);
      fputs("lw ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      if (instr->label != NULL) {
        if (instr->rb == R0 && instr->imm == 0) {
          fputc('[', out);
          write_label_or_imm(out, instr->label, instr->imm);
          fputc(']', out);
        } else {
          write_mem_operand_label(out, instr->rb, instr->label, instr->imm);
        }
      } else {
        write_mem_operand(out, instr->rb, instr->imm);
      }
      fputc('\n', out);
      return true;
    case MACHINE_SDA:
      write_tab(out);
      fputs("sda ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_mem_operand(out, instr->rb, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_LDA:
      write_tab(out);
      fputs("lda ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_mem_operand(out, instr->rb, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_SD:
      write_tab(out);
      fputs("sd ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      if (instr->label != NULL) {
        if (instr->rb == R0 && instr->imm == 0) {
          fputc('[', out);
          write_label_or_imm(out, instr->label, instr->imm);
          fputc(']', out);
        } else {
          write_mem_operand_label(out, instr->rb, instr->label, instr->imm);
        }
      } else {
        write_mem_operand(out, instr->rb, instr->imm);
      }
      fputc('\n', out);
      return true;
    case MACHINE_LD:
      write_tab(out);
      fputs("ld ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      if (instr->label != NULL) {
        if (instr->rb == R0 && instr->imm == 0) {
          fputc('[', out);
          write_label_or_imm(out, instr->label, instr->imm);
          fputc(']', out);
        } else {
          write_mem_operand_label(out, instr->rb, instr->label, instr->imm);
        }
      } else {
        write_mem_operand(out, instr->rb, instr->imm);
      }
      fputc('\n', out);
      return true;
    case MACHINE_SBA:
      write_tab(out);
      fputs("sba ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_mem_operand(out, instr->rb, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_LBA:
      write_tab(out);
      fputs("lba ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_mem_operand(out, instr->rb, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_SB:
      write_tab(out);
      fputs("sb ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      if (instr->label != NULL) {
        if (instr->rb == R0 && instr->imm == 0) {
          fputc('[', out);
          write_label_or_imm(out, instr->label, instr->imm);
          fputc(']', out);
        } else {
          write_mem_operand_label(out, instr->rb, instr->label, instr->imm);
        }
      } else {
        write_mem_operand(out, instr->rb, instr->imm);
      }
      fputc('\n', out);
      return true;
    case MACHINE_LB:
      write_tab(out);
      fputs("lb ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      if (instr->label != NULL) {
        if (instr->rb == R0 && instr->imm == 0) {
          fputc('[', out);
          write_label_or_imm(out, instr->label, instr->imm);
          fputc(']', out);
        } else {
          write_mem_operand_label(out, instr->rb, instr->label, instr->imm);
        }
      } else {
        write_mem_operand(out, instr->rb, instr->imm);
      }
      fputc('\n', out);
      return true;
    case MACHINE_BR:
      write_tab(out);
      if (instr->ra != 0 || instr->rb != 0) {
        fputs("br ", out);
        write_reg(out, instr->ra);
        fputs(", ", out);
        write_reg(out, instr->rb);
        fputc('\n', out);
      } else {
        stream_printf1_int(out, "br %d\n", instr->imm);
      }
      return true;
    case MACHINE_BZ:
      write_tab(out);
      fputs("bz ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_BNZ:
      write_tab(out);
      fputs("bnz ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_BS:
      write_tab(out);
      fputs("bs ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_BNS:
      write_tab(out);
      fputs("bns ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_BC:
      write_tab(out);
      fputs("bc ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_BNC:
      write_tab(out);
      fputs("bnc ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_BO:
      write_tab(out);
      fputs("bo ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_BNO:
      write_tab(out);
      fputs("bno ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_BPS:
      write_tab(out);
      fputs("bps ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_BNPS:
      write_tab(out);
      fputs("bnps ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_BG:
      write_tab(out);
      fputs("bg ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_BGE:
      write_tab(out);
      fputs("bge ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_BL:
      write_tab(out);
      fputs("bl ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_BLE:
      write_tab(out);
      fputs("ble ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_BA:
      write_tab(out);
      fputs("ba ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_BAE:
      write_tab(out);
      fputs("bae ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_BB:
      write_tab(out);
      fputs("bb ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_BBE:
      write_tab(out);
      fputs("bbe ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_BRA:
      write_tab(out);
      fputs("bra ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_BZA:
      write_tab(out);
      fputs("bza ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_BNZA:
      write_tab(out);
      fputs("bnza ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_BSA:
      write_tab(out);
      fputs("bsa ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_BNSA:
      write_tab(out);
      fputs("bnsa ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_BCA:
      write_tab(out);
      fputs("bca ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_BNCA:
      write_tab(out);
      fputs("bnca ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_BOA:
      write_tab(out);
      fputs("boa ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_BNOA:
      write_tab(out);
      fputs("bnoa ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_BPSA:
      write_tab(out);
      fputs("bpa ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_BNPSA:
      write_tab(out);
      fputs("bnpa ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_BGA:
      write_tab(out);
      fputs("bga ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_BGEA:
      write_tab(out);
      fputs("bgea ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_BLA:
      write_tab(out);
      fputs("bla ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_BLEA:
      write_tab(out);
      fputs("blea ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_BAA:
      write_tab(out);
      fputs("baa ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_BAEA:
      write_tab(out);
      fputs("baea ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_BBA:
      write_tab(out);
      fputs("bba ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_BBEA:
      write_tab(out);
      fputs("bbea ", out);
      write_reg(out, instr->ra);
      fputs(", ", out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_SYS:
      write_tab(out);
      (void)instr;
      fputs("trap\n", out);
      return true;
    case MACHINE_NOP:
      write_tab(out);
      fputs("nop\n", out);
      return true;
    case MACHINE_PUSH:
      write_tab(out);
      fputs("push ", out);
      write_reg(out, instr->ra);
      fputc('\n', out);
      return true;
    case MACHINE_POP:
      write_tab(out);
      fputs("pop ", out);
      write_reg(out, instr->ra);
      fputc('\n', out);
      return true;
    case MACHINE_PUSHD:
      write_tab(out);
      fputs("pshd ", out);
      write_reg(out, instr->ra);
      fputc('\n', out);
      return true;
    case MACHINE_POPD:
      write_tab(out);
      fputs("popd ", out);
      write_reg(out, instr->ra);
      fputc('\n', out);
      return true;
    case MACHINE_PUSHB:
      write_tab(out);
      fputs("pshb ", out);
      write_reg(out, instr->ra);
      fputc('\n', out);
      return true;
    case MACHINE_POPB:
      write_tab(out);
      fputs("popb ", out);
      write_reg(out, instr->ra);
      fputc('\n', out);
      return true;
    case MACHINE_CALL:
      write_tab(out);
      fputs("call ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_RET:
      write_tab(out);
      fputs("ret\n", out);
      return true;
    case MACHINE_JMP:
      write_tab(out);
      fputs("jmp ", out);
      write_label_or_imm(out, instr->label, instr->imm);
      fputc('\n', out);
      return true;
    case MACHINE_CMP:
      write_tab(out);
      fputs("cmp ", out);
      write_reg(out, instr->ra);
      fputc(' ', out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_TNCB:
      write_tab(out);
      fputs("tncb ", out);
      write_reg(out, instr->ra);
      fputc(' ', out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_TNCD:
      write_tab(out);
      fputs("tncd ", out);
      write_reg(out, instr->ra);
      fputc(' ', out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_SXTB:
      write_tab(out);
      fputs("sxtb ", out);
      write_reg(out, instr->ra);
      fputc(' ', out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    case MACHINE_SXTD:
      write_tab(out);
      fputs("sxtd ", out);
      write_reg(out, instr->ra);
      fputc(' ', out);
      write_reg(out, instr->rb);
      fputc('\n', out);
      return true;
    default:
      stream_printf1_int(stderr,
                         "Compiler Error: machine_print: unknown instruction type %d\n",
                         (int)instr->type);
      return false;
  }
}

bool write_machine_prog_to_file(struct MachineProg* prog, char* path) {
  if (prog == NULL || path == NULL) {
    fputs("Compiler Error: machine_print: invalid write request\n", stderr);
    return false;
  }

  FILE* out = fopen(path, "w");
  if (out == NULL) {
    stream_printf1_str(stderr,
                       "Compiler Error: machine_print: failed to open %s\n",
                       path);
    return false;
  }

  for (struct MachineInstr* cur = prog->head; cur != NULL; cur = cur->next) {
    if (!write_machine_instr(out, cur)) {
      fclose(out);
      return false;
    }
  }

  if (fclose(out) != 0) {
    stream_printf1_str(stderr,
                       "Compiler Error: machine_print: failed to close %s\n",
                       path);
    return false;
  }

  return true;
}
