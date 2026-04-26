#include "codegen.h"
#include "asm_gen.h"
#include "arena.h"
#include "../crt/stdio.h"
#include "../crt/print.h"
#include "../crt/stdlib.h"
#include "../crt/stdint.h"

// Purpose: Allocate a machine instruction node with predictable defaults.
// Inputs: type is the machine instruction opcode to emit.
// Outputs: Returns a zeroed instruction node owned by the arena.
// Invariants/Assumptions: arena has been initialized before codegen runs.
static struct MachineInstr* alloc_machine_instr(enum MachineInstrType type) {
  struct MachineInstr* instr = arena_alloc(sizeof(struct MachineInstr));
  instr->type = type;
  instr->ra = 0;
  instr->rb = 0;
  instr->rc = 0;
  instr->imm = 0;
  instr->label = NULL;
  instr->debug_loc = NULL;
  instr->debug_name = NULL;
  instr->debug_offset = 0;
  instr->exc = 0;
  instr->next = NULL;
  return instr;
}

// Purpose: Append an instruction node to a single-instruction list builder.
// Inputs: head/tail track the list, instr is the node to append.
// Outputs: Updates head/tail to include instr.
// Invariants/Assumptions: instr is a standalone node (next == NULL).
static void append_instr(struct MachineInstr** head,
                         struct MachineInstr** tail,
                         struct MachineInstr* instr) {
  if (*head == NULL) {
    *head = instr;
    *tail = instr;
    return;
  }
  (*tail)->next = instr;
  *tail = instr;
}

// Purpose: Materialize a data label address into a register, optionally with a byte offset.
// Inputs: addr_reg receives the absolute address; pc_reg is a scratch for PC; label is the data symbol.
// Outputs: Appends machine instructions to compute the address.
// Invariants/Assumptions: addr_reg and pc_reg are distinct scratch registers.
static void emit_label_address(struct MachineInstr** head,
                               struct MachineInstr** tail,
                               enum Reg addr_reg,
                               enum Reg pc_reg,
                               struct Slice* label,
                               int offset) {
  struct MachineInstr* movi = alloc_machine_instr(MACHINE_MOVI);
  movi->ra = addr_reg;
  movi->label = label;
  append_instr(head, tail, movi);

  struct MachineInstr* br = alloc_machine_instr(MACHINE_BR);
  br->ra = pc_reg;
  br->rb = R0;
  append_instr(head, tail, br);

  struct MachineInstr* add_pc = alloc_machine_instr(MACHINE_ADD);
  add_pc->ra = addr_reg;
  add_pc->rb = addr_reg;
  add_pc->rc = pc_reg;
  append_instr(head, tail, add_pc);

  if (offset != 0) {
    struct MachineInstr* add_off = alloc_machine_instr(MACHINE_ADD);
    add_off->ra = addr_reg;
    add_off->rb = addr_reg;
    add_off->rc = R0;
    add_off->imm = offset;
    append_instr(head, tail, add_off);
  }
}

// Purpose: Load from a data label (optionally with an offset) into a register.
// Inputs: dst_reg receives the loaded value; data is the label operand.
// Outputs: Appends the appropriate load sequence.
// Invariants/Assumptions: data->type is OPERAND_DATA.
// Purpose: Find the first source location marker in a function body.
// Inputs: instrs is the ASM instruction list for the function body.
// Outputs: Returns the loc pointer for the first ASM_BOUNDARY, or NULL if none.
// Invariants/Assumptions: instrs is a well-formed list produced by asm_gen.
static char* find_function_entry_loc(struct AsmInstr* instrs) {
  for (struct AsmInstr* cur = instrs; cur != NULL; cur = cur->next) {
    if (cur->type == ASM_BOUNDARY) {
      return cur->loc;
    }
  }
  return NULL;
}

// Purpose: Print a slice to the selected file descriptor.
// Inputs: slice may be NULL; otherwise points to a valid slice.
// Outputs: Writes a best-effort identifier representation to fd.
// Invariants/Assumptions: slice->start may be non-null-terminated.
static void codegen_fdprint_slice(int fd, struct Slice* slice) {
  int args[2];

  if (slice == NULL || slice->start == NULL) {
    fdputs(fd, "<null>");
    return;
  }
  args[0] = (int)slice->len;
  args[1] = (int)slice->start;
  fdprintf(fd, "%.*s", args);
}

// Purpose: Emit the shared suffix for codegen diagnostics.
// Inputs: func_name is the current function (may be NULL), instr_type is the ASM opcode.
// Outputs: Writes instruction/function context to stderr.
// Invariants/Assumptions: Called after the main diagnostic message text.
static void codegen_error_suffix(struct Slice* func_name, enum AsmInstrType instr_type) {
  int args[1];

  args[0] = (int)instr_type;
  if (func_name != NULL) {
    fdprintf(STDERR, " (asm=%d, func=", args);
    codegen_fdprint_slice(STDERR, func_name);
    fdputs(STDERR, ")\n");
  } else {
    fdprintf(STDERR, " (asm=%d)\n", args);
  }
}

// Purpose: Report a codegen error with context, then exit.
// Inputs: func_name is the current function (may be NULL), instr_type is the ASM opcode.
// Outputs: Prints an actionable message to stderr and terminates.
// Invariants/Assumptions: The message helpers below cover every call shape used by bootstrap bcc.
static void codegen_error0(struct Slice* func_name,
                           enum AsmInstrType instr_type,
                           char* message) {
  fdputs(STDERR, "Compiler Error: codegen: ");
  fdputs(STDERR, message);
  codegen_error_suffix(func_name, instr_type);
  exit(1);
}

static void codegen_error1_int(struct Slice* func_name,
                               enum AsmInstrType instr_type,
                               char* fmt,
                               int arg0) {
  int args[1];

  args[0] = arg0;
  fdputs(STDERR, "Compiler Error: codegen: ");
  fdprintf(STDERR, fmt, args);
  codegen_error_suffix(func_name, instr_type);
  exit(1);
}

static void codegen_error1_size(struct Slice* func_name,
                                enum AsmInstrType instr_type,
                                char* fmt,
                                size_t arg0) {
  int args[1];

  args[0] = (int)arg0;
  fdputs(STDERR, "Compiler Error: codegen: ");
  fdprintf(STDERR, fmt, args);
  codegen_error_suffix(func_name, instr_type);
  exit(1);
}

static void codegen_error1_slice(struct Slice* func_name,
                                 enum AsmInstrType instr_type,
                                 char* fmt,
                                 struct Slice* slice) {
  int args[2];

  if (slice == NULL || slice->start == NULL) {
    args[0] = 6;
    args[1] = (int)"<null>";
  } else {
    args[0] = (int)slice->len;
    args[1] = (int)slice->start;
  }
  fdputs(STDERR, "Compiler Error: codegen: ");
  fdprintf(STDERR, fmt, args);
  codegen_error_suffix(func_name, instr_type);
  exit(1);
}

static void codegen_error2_int(struct Slice* func_name,
                               enum AsmInstrType instr_type,
                               char* fmt,
                               int arg0,
                               int arg1) {
  int args[2];

  args[0] = arg0;
  args[1] = arg1;
  fdputs(STDERR, "Compiler Error: codegen: ");
  fdprintf(STDERR, fmt, args);
  codegen_error_suffix(func_name, instr_type);
  exit(1);
}

// Purpose: Select a scratch register that avoids two disallowed registers.
// Inputs: avoid_a/avoid_b are registers that must not be selected.
// Outputs: Returns a scratch register distinct from avoid_a and avoid_b.
// Invariants/Assumptions: At least one scratch register remains available.
static enum Reg pick_scratch_reg(struct Slice* func_name,
                                 enum AsmInstrType instr_type,
                                 enum Reg avoid_a,
                                 enum Reg avoid_b) {
  if (kScratchRegA != avoid_a && kScratchRegA != avoid_b) {
    return kScratchRegA;
  }
  if (kScratchRegB != avoid_a && kScratchRegB != avoid_b) {
    return kScratchRegB;
  }
  if (kScratchRegC != avoid_a && kScratchRegC != avoid_b) {
    return kScratchRegC;
  }
  codegen_error2_int(func_name, instr_type,
                     "no scratch register available (avoid=%d,%d)",
                     (int)avoid_a, (int)avoid_b);
  return kScratchRegA;
}

// Builtin helper names referenced by codegen-generated call sequences.
static struct Slice kBuiltinSmul = {"smul", 4};
static struct Slice kBuiltinSdiv = {"sdiv", 4};
static struct Slice kBuiltinSmod = {"smod", 4};
static struct Slice kBuiltinUmul = {"umul", 4};
static struct Slice kBuiltinUdiv = {"udiv", 4};
static struct Slice kBuiltinUmod = {"umod", 4};
static struct Slice kBuiltinSLeftShift = {"sleft_shift", 11};
static struct Slice kBuiltinSRightShift = {"sright_shift", 12};
static struct Slice kBuiltinULeftShift = {"uleft_shift", 11};
static struct Slice kBuiltinURightShift = {"uright_shift", 12};
static struct Slice kFunctionEpilogueLabel = {"Function Epilogue", 17};
static struct Slice kFunctionPrologueLabel = {"Function Prologue", 17};
static struct Slice kFunctionBodyLabel = {"Function Body", 13};

// Purpose: Emit a call sequence for a binary builtin that expects args in R1/R2.
// Inputs: head/tail are the instruction list; label identifies the builtin entry.
// Outputs: Appends mov/call/mov to set args and capture the result in scratch A.
// Invariants/Assumptions: Uses caller-saved registers R1/R2 to pass arguments.
static void append_builtin_call(struct MachineInstr** head,
                                struct MachineInstr** tail,
                                struct Slice* label) {
  struct MachineInstr* mov_a = alloc_machine_instr(MACHINE_MOV);
  mov_a->ra = R1;
  mov_a->rb = kScratchRegA;
  append_instr(head, tail, mov_a);

  struct MachineInstr* mov_b = alloc_machine_instr(MACHINE_MOV);
  mov_b->ra = R2;
  mov_b->rb = kScratchRegB;
  append_instr(head, tail, mov_b);

  struct MachineInstr* call = alloc_machine_instr(MACHINE_CALL);
  call->label = label;
  append_instr(head, tail, call);

  struct MachineInstr* mov_result = alloc_machine_instr(MACHINE_MOV);
  mov_result->ra = kScratchRegA;
  mov_result->rb = R1;
  append_instr(head, tail, mov_result);
}

// Constants derived from ABI stack layout and short-branch sequencing (byte offsets).
static int kCondJumpBranchSkip = 4;
static int kCondJumpJmpSkip = 12;
static int kZeroOffset = 0;
static int kSavedBpOffset = 0;
static int kSavedRaOffset = 4;
static int kEpilogueStackBytes = 8;

// Purpose: Load from a data label (optionally with an offset) into a register.
// Inputs: dst_reg receives the loaded value; data is the label operand.
// Outputs: Appends the appropriate load sequence.
// Invariants/Assumptions: data->type is OPERAND_DATA.
static void emit_data_load(struct MachineInstr** head,
                           struct MachineInstr** tail,
                           struct Slice* func_name,
                           enum AsmInstrType instr_type,
                           enum Reg dst_reg,
                           struct Operand* data) {
  if (data->lit_value != 0) {
    enum Reg pc_reg = (dst_reg == kScratchRegA) ? kScratchRegB : kScratchRegA;
    emit_label_address(head, tail, dst_reg, pc_reg, data->pseudo, data->lit_value);
    struct MachineInstr* load;
    switch (data->asm_type->type) {
      case BYTE:
        load = alloc_machine_instr(MACHINE_LBA);
        break;
      case DOUBLE:
        load = alloc_machine_instr(MACHINE_LDA);
        break;
      case WORD:
        load = alloc_machine_instr(MACHINE_LWA);
        break;
      default:
        codegen_error1_int(func_name, instr_type,
                           "unsupported asm type %d for data operand load",
                           (int)data->asm_type->type);
        return;
    }
    load->ra = dst_reg;
    load->rb = dst_reg;
    load->imm = 0;
    append_instr(head, tail, load);
  } else {
    struct MachineInstr* load;
    switch (data->asm_type->type) {
      case BYTE:
        load = alloc_machine_instr(MACHINE_LB);
        break;
      case DOUBLE:
        load = alloc_machine_instr(MACHINE_LD);
        break;
      case WORD:
        load = alloc_machine_instr(MACHINE_LW);
        break;
      default:
        codegen_error1_int(func_name, instr_type,
                           "unsupported asm type %d for data operand load",
                           (int)data->asm_type->type);
        return;
    }
    load->ra = dst_reg;
    load->rb = R0;
    load->imm = kZeroOffset;
    load->label = data->pseudo;
    append_instr(head, tail, load);
  }
}

static struct MachineInstr* make_data(struct InitList* init, struct AsmType* type);

// Purpose: Load the generic instruction sources into the scratch registers.
// Inputs: cur is the ASM instruction being lowered; head/tail collect emitted machine ops.
// Outputs: Appends loads/moves so scratch A/B hold the instruction sources.
// Invariants/Assumptions: Generic lowering uses at most two explicit source operands.
static void append_generic_source_loads(struct MachineInstr** head,
                                        struct MachineInstr** tail,
                                        struct Slice* func_name,
                                        struct AsmInstr* cur) {
  size_t src_count = 0;
  struct Operand** srcs = get_srcs(cur, &src_count);

  if (src_count == 2) {
    struct Operand* a = srcs[0];
    struct Operand* b = srcs[1];
    if (a->type == OPERAND_REG) {
      struct MachineInstr* mov = alloc_machine_instr(MACHINE_MOV);
      mov->ra = kScratchRegA;
      mov->rb = a->reg;
      append_instr(head, tail, mov);
    } else if (a->type == OPERAND_MEMORY) {
      struct MachineInstr* load;
      switch (a->asm_type->type) {
        case BYTE:
          load = alloc_machine_instr(MACHINE_LBA);
          break;
        case DOUBLE:
          load = alloc_machine_instr(MACHINE_LDA);
          break;
        case WORD:
          load = alloc_machine_instr(MACHINE_LWA);
          break;
        default:
          codegen_error1_int(func_name, cur->type,
                             "unsupported asm type %d for source operand",
                             (int)cur->src1->asm_type->type);
          break;
      }
      load->ra = kScratchRegA;
      load->rb = a->reg;
      load->imm = a->lit_value;
      append_instr(head, tail, load);
    } else if (a->type == OPERAND_LIT) {
      struct MachineInstr* movi = alloc_machine_instr(MACHINE_MOVI);
      movi->ra = kScratchRegA;
      movi->imm = a->lit_value;
      append_instr(head, tail, movi);
    } else if (a->type == OPERAND_DATA) {
      emit_data_load(head, tail, func_name, cur->type, kScratchRegA, a);
    } else {
      codegen_error1_int(func_name, cur->type,
                         "invalid first source operand type %d; expected Reg, Memory, Lit, or Data",
                         (int)a->type);
    }

    if (b->type == OPERAND_REG) {
      struct MachineInstr* mov = alloc_machine_instr(MACHINE_MOV);
      mov->ra = kScratchRegB;
      mov->rb = b->reg;
      append_instr(head, tail, mov);
    } else if (b->type == OPERAND_MEMORY) {
      struct MachineInstr* load;
      switch (b->asm_type->type) {
        case BYTE:
          load = alloc_machine_instr(MACHINE_LBA);
          break;
        case DOUBLE:
          load = alloc_machine_instr(MACHINE_LDA);
          break;
        case WORD:
          load = alloc_machine_instr(MACHINE_LWA);
          break;
        default:
          codegen_error1_int(func_name, cur->type,
                             "unsupported asm type %d for source operand",
                             (int)cur->src2->asm_type->type);
          break;
      }
      load->ra = kScratchRegB;
      load->rb = b->reg;
      load->imm = b->lit_value;
      append_instr(head, tail, load);
    } else if (b->type == OPERAND_LIT) {
      struct MachineInstr* movi = alloc_machine_instr(MACHINE_MOVI);
      movi->ra = kScratchRegB;
      movi->imm = b->lit_value;
      append_instr(head, tail, movi);
    } else if (b->type == OPERAND_DATA) {
      emit_data_load(head, tail, func_name, cur->type, kScratchRegB, b);
    } else {
      codegen_error1_int(func_name, cur->type,
                         "invalid second source operand type %d; expected Reg, Memory, Lit, or Data",
                         (int)b->type);
    }
    return;
  }

  if (src_count == 1) {
    struct Operand* a = srcs[0];
    if (a->type == OPERAND_REG) {
      struct MachineInstr* mov = alloc_machine_instr(MACHINE_MOV);
      mov->ra = kScratchRegA;
      mov->rb = a->reg;
      append_instr(head, tail, mov);
    } else if (a->type == OPERAND_MEMORY) {
      struct MachineInstr* load;
      switch (a->asm_type->type) {
        case BYTE:
          load = alloc_machine_instr(MACHINE_LBA);
          break;
        case DOUBLE:
          load = alloc_machine_instr(MACHINE_LDA);
          break;
        case WORD:
          load = alloc_machine_instr(MACHINE_LWA);
          break;
        default:
          codegen_error1_int(func_name, cur->type,
                             "unsupported asm type %d for source operand",
                             (int)cur->src1->asm_type->type);
          break;
      }
      load->ra = kScratchRegA;
      load->rb = a->reg;
      load->imm = a->lit_value;
      append_instr(head, tail, load);
    } else if (a->type == OPERAND_LIT) {
      struct MachineInstr* movi = alloc_machine_instr(MACHINE_MOVI);
      movi->ra = kScratchRegA;
      movi->imm = a->lit_value;
      append_instr(head, tail, movi);
    } else if (a->type == OPERAND_DATA) {
      emit_data_load(head, tail, func_name, cur->type, kScratchRegA, a);
    } else {
      codegen_error1_int(func_name, cur->type,
                         "invalid source operand type %d; expected Reg, Memory, Lit, or Data",
                         (int)a->type);
    }
  }
}

struct MachineProg* instr_to_machine(struct Slice* func_name, struct AsmInstr* instr){
  // Uses R9/R10/R11 as scratch registers to avoid clobbering argument registers.
  struct MachineProg* machine_prog = arena_alloc(sizeof(struct MachineProg));
  machine_prog->head = NULL;
  machine_prog->tail = NULL;

  for (struct AsmInstr* cur = instr; cur != NULL; cur = cur->next) {
    struct MachineInstr* head = NULL;
    struct MachineInstr* tail = NULL;
    bool handled = false;

    if (cur->type == ASM_MOV && cur->dst != NULL && cur->src1 != NULL) {
      if (cur->dst->type == OPERAND_REG && cur->src1->type == OPERAND_REG) {
        // Machine: Mov rDst, rSrc
        struct MachineInstr* mov = alloc_machine_instr(MACHINE_MOV);
        mov->ra = cur->dst->reg;
        mov->rb = cur->src1->reg;
        append_instr(&head, &tail, mov);
        handled = true;
      } else if (cur->dst->type == OPERAND_REG && cur->src1->type == OPERAND_LIT) {
        // Machine: Movi rDst, imm
        struct MachineInstr* movi = alloc_machine_instr(MACHINE_MOVI);
        movi->ra = cur->dst->reg;
        movi->imm = cur->src1->lit_value;
        append_instr(&head, &tail, movi);
        handled = true;
      } else if (cur->dst->type == OPERAND_MEMORY && cur->src1->type == OPERAND_REG) {
        // Machine: Swa rSrc, [rBase, off]
        struct MachineInstr* store;
        switch (cur->dst->asm_type->type) {
          case BYTE:
            // byte store
            store = alloc_machine_instr(MACHINE_SBA);
            break;
          case DOUBLE:
            // double store
            store = alloc_machine_instr(MACHINE_SDA);
            break;
          case WORD:
            // word store
            store = alloc_machine_instr(MACHINE_SWA);
            break;
          default:
            codegen_error1_int(func_name, cur->type,
                               "unsupported asm type %d for memory operand store",
                               (int)cur->dst->asm_type->type);
            break;
        }
        store->ra = cur->src1->reg;
        store->rb = cur->dst->reg;
        store->imm = cur->dst->lit_value;
        append_instr(&head, &tail, store);
        handled = true;
      } else if (cur->dst->type == OPERAND_DATA && cur->src1->type == OPERAND_REG) {
        if (cur->dst->lit_value != 0) {
          // Store through a computed absolute address for label+offset.
          emit_label_address(&head, &tail, kScratchRegB, kScratchRegA,
                             cur->dst->pseudo, cur->dst->lit_value);
          struct MachineInstr* store;
          switch (cur->dst->asm_type->type) {
            case BYTE:
              store = alloc_machine_instr(MACHINE_SBA);
              break;
            case DOUBLE:
              store = alloc_machine_instr(MACHINE_SDA);
              break;
            case WORD:
              store = alloc_machine_instr(MACHINE_SWA);
              break;
            default:
              codegen_error1_int(func_name, cur->type,
                                 "unsupported asm type %d for data operand store",
                                 (int)cur->dst->asm_type->type);
              break;
          }
          store->ra = cur->src1->reg;
          store->rb = kScratchRegB;
          store->imm = 0;
          append_instr(&head, &tail, store);
        } else {
          // Machine: Store rSrc, [label]
          struct MachineInstr* store;
          switch (cur->dst->asm_type->type) {
            case BYTE:
              // byte store
              store = alloc_machine_instr(MACHINE_SB);
              break;
            case DOUBLE:
              // double store
              store = alloc_machine_instr(MACHINE_SD);
              break;
            case WORD:
              // word store
              store = alloc_machine_instr(MACHINE_SW);
              break;
            default:
              codegen_error1_int(func_name, cur->type,
                                 "unsupported asm type %d for data operand store",
                                 (int)cur->dst->asm_type->type);
              break;
          }
          store->ra = cur->src1->reg;
          store->rb = R0;
          store->imm = kZeroOffset;
          store->label = cur->dst->pseudo;
          append_instr(&head, &tail, store);
        }
        handled = true;
      } else if (cur->dst->type == OPERAND_MEMORY &&
                 (cur->src1->type == OPERAND_DATA ||
                  cur->src1->type == OPERAND_LIT ||
                  cur->src1->type == OPERAND_MEMORY)) {
        enum Reg value_reg = kScratchRegA;
        if (cur->src1->type == OPERAND_MEMORY) {
          value_reg = pick_scratch_reg(func_name, cur->type, cur->dst->reg, cur->src1->reg);
        } else {
          value_reg = pick_scratch_reg(func_name, cur->type, cur->dst->reg, R0);
        }
        if (cur->src1->type == OPERAND_DATA) {
          if (cur->src1->lit_value == 0) {
            struct MachineInstr* load;
            switch (cur->src1->asm_type->type) {
              case BYTE:
                load = alloc_machine_instr(MACHINE_LB);
                break;
              case DOUBLE:
                load = alloc_machine_instr(MACHINE_LD);
                break;
              case WORD:
                load = alloc_machine_instr(MACHINE_LW);
                break;
              default:
                codegen_error1_int(func_name, cur->type,
                                   "unsupported asm type %d for data operand load",
                                   (int)cur->src1->asm_type->type);
                break;
            }
            load->ra = value_reg;
            load->rb = R0;
            load->imm = kZeroOffset;
            load->label = cur->src1->pseudo;
            append_instr(&head, &tail, load);
          } else {
            enum Reg pc_reg = pick_scratch_reg(func_name, cur->type, cur->dst->reg, value_reg);
            emit_label_address(&head, &tail, value_reg, pc_reg,
                               cur->src1->pseudo, cur->src1->lit_value);
            struct MachineInstr* load;
            switch (cur->src1->asm_type->type) {
              case BYTE:
                load = alloc_machine_instr(MACHINE_LBA);
                break;
              case DOUBLE:
                load = alloc_machine_instr(MACHINE_LDA);
                break;
              case WORD:
                load = alloc_machine_instr(MACHINE_LWA);
                break;
              default:
                codegen_error1_int(func_name, cur->type,
                                   "unsupported asm type %d for data operand load",
                                   (int)cur->src1->asm_type->type);
                break;
            }
            load->ra = value_reg;
            load->rb = value_reg;
            load->imm = 0;
            append_instr(&head, &tail, load);
          }
        } else if (cur->src1->type == OPERAND_LIT) {
          struct MachineInstr* movi = alloc_machine_instr(MACHINE_MOVI);
          movi->ra = value_reg;
          movi->imm = cur->src1->lit_value;
          append_instr(&head, &tail, movi);
        } else if (cur->src1->type == OPERAND_MEMORY) {
          struct MachineInstr* load;
          switch (cur->src1->asm_type->type) {
            case BYTE:
              load = alloc_machine_instr(MACHINE_LBA);
              break;
            case DOUBLE:
              load = alloc_machine_instr(MACHINE_LDA);
              break;
            case WORD:
              load = alloc_machine_instr(MACHINE_LWA);
              break;
            default:
              codegen_error1_int(func_name, cur->type,
                                 "unsupported asm type %d for memory operand load",
                                 (int)cur->src1->asm_type->type);
              break;
          }
          load->ra = value_reg;
          load->rb = cur->src1->reg;
          load->imm = cur->src1->lit_value;
          append_instr(&head, &tail, load);
        }

        struct MachineInstr* store;
        switch (cur->dst->asm_type->type) {
          case BYTE:
            store = alloc_machine_instr(MACHINE_SBA);
            break;
          case DOUBLE:
            store = alloc_machine_instr(MACHINE_SDA);
            break;
          case WORD:
            store = alloc_machine_instr(MACHINE_SWA);
            break;
          default:
            codegen_error1_int(func_name, cur->type,
                               "unsupported asm type %d for memory operand store",
                               (int)cur->dst->asm_type->type);
            break;
        }
        store->ra = value_reg;
        store->rb = cur->dst->reg;
        store->imm = cur->dst->lit_value;
        append_instr(&head, &tail, store);
        handled = true;
      } 
      else if (cur->dst->type == OPERAND_REG && cur->src1->type == OPERAND_MEMORY) {
         // Machine: Lwa rDst, [rBase, off]
         struct MachineInstr* load;
         switch (cur->src1->asm_type->type) {
           case BYTE:
             // byte load
             load = alloc_machine_instr(MACHINE_LBA);
             break;
           case DOUBLE:
             // double load
             load = alloc_machine_instr(MACHINE_LDA);
             break;
           case WORD:
             // word load
             load = alloc_machine_instr(MACHINE_LWA);
             break;
           default:
             codegen_error1_int(func_name, cur->type,
                                "unsupported asm type %d for memory operand load",
                                (int)cur->src1->asm_type->type);
             break;
         }
         load->ra = cur->dst->reg;
         load->rb = cur->src1->reg;
         load->imm = cur->src1->lit_value;
         append_instr(&head, &tail, load);
         handled = true;
       } 
      else if (cur->dst->type == OPERAND_REG && cur->src1->type == OPERAND_DATA) {
        emit_data_load(&head, &tail, func_name, cur->type, cur->dst->reg, cur->src1);
        handled = true;
      }
      
      if (!handled && cur->type == ASM_PUSH && cur->src1 != NULL) {
        if (cur->src1->type == OPERAND_REG) {
          // Machine: Push rSrc
          struct MachineInstr* push = alloc_machine_instr(MACHINE_PUSH);
          push->ra = cur->src1->reg;
          append_instr(&head, &tail, push);
          handled = true;
        }
      }
    }

    if (!handled && cur->type == ASM_GET_ADDRESS && cur->dst != NULL && cur->src1 != NULL) {
      if (cur->dst->type == OPERAND_MEMORY && cur->src1->type == OPERAND_MEMORY) {
        // Machine: Add rTmp, rBase, off; Swa rTmp, [rDstBase, dstOff]
        struct MachineInstr* add = alloc_machine_instr(MACHINE_ADD);
        add->ra = kScratchRegB;
        add->rb = cur->src1->reg;
        add->imm = cur->src1->lit_value;
        append_instr(&head, &tail, add);
        struct MachineInstr* sw = alloc_machine_instr(MACHINE_SWA);
        sw->ra = kScratchRegB;
        sw->rb = cur->dst->reg;
        sw->imm = cur->dst->lit_value;
        append_instr(&head, &tail, sw);
        handled = true;
      } else if (cur->dst->type == OPERAND_MEMORY && cur->src1->type == OPERAND_DATA) {
        emit_label_address(&head, &tail, kScratchRegB, kScratchRegA,
                           cur->src1->pseudo, cur->src1->lit_value);
        struct MachineInstr* sw = alloc_machine_instr(MACHINE_SWA);
        sw->ra = kScratchRegB;
        sw->rb = cur->dst->reg;
        sw->imm = cur->dst->lit_value;
        append_instr(&head, &tail, sw);
        handled = true;
      } else if (cur->dst->type == OPERAND_REG && cur->src1->type == OPERAND_MEMORY) {
        struct MachineInstr* add = alloc_machine_instr(MACHINE_ADD);
        add->ra = cur->dst->reg;
        add->rb = cur->src1->reg;
        add->imm = cur->src1->lit_value;
        append_instr(&head, &tail, add);
        handled = true;
      } else if (cur->dst->type == OPERAND_REG && cur->src1->type == OPERAND_DATA) {
        enum Reg pc_reg = (cur->dst->reg == kScratchRegA) ? kScratchRegB : kScratchRegA;
        emit_label_address(&head, &tail, cur->dst->reg, pc_reg,
                           cur->src1->pseudo, cur->src1->lit_value);
        handled = true;
      }
    }

    if (!handled) {
      // Generic lowering: load sources into scratch regs, emit op, then store scratch A.
      append_generic_source_loads(&head, &tail, func_name, cur);

      switch (cur->type) {
        case ASM_MOV:
          break;
        case ASM_CMP: {
          // Machine: Cmp rScratchA, rScratchB
          struct MachineInstr* cmp = alloc_machine_instr(MACHINE_CMP);
          cmp->ra = kScratchRegA;
          cmp->rb = kScratchRegB;
          append_instr(&head, &tail, cmp);
          break;
        }
        case ASM_UNARY:
          if (cur->unary_op == COMPLEMENT) {
            // Machine: Not rScratchA, rScratchA
            struct MachineInstr* not_instr = alloc_machine_instr(MACHINE_NOT);
            not_instr->ra = kScratchRegA;
            not_instr->rb = kScratchRegA;
            append_instr(&head, &tail, not_instr);
          } else if (cur->unary_op == NEGATE) {
            // Machine: Sub rScratchA, R0, rScratchA
            struct MachineInstr* sub = alloc_machine_instr(MACHINE_SUB);
            sub->ra = kScratchRegA;
            sub->rb = R0;
            sub->rc = kScratchRegA;
            append_instr(&head, &tail, sub);
          } else if (cur->unary_op == UNARY_PLUS) {
            // no-op
          } else {
            codegen_error1_int(func_name, cur->type,
                               "unsupported unary op %d; expected COMPLEMENT, NEGATE, or UNARY_PLUS",
                               (int)cur->unary_op);
          }
          break;
        case ASM_BINARY:
          switch (cur->alu_op) {
            case ALU_ADD: {
              // Machine: Add rScratchA, rScratchA, rScratchB
              struct MachineInstr* add = alloc_machine_instr(MACHINE_ADD);
              add->ra = kScratchRegA;
              add->rb = kScratchRegA;
              add->rc = kScratchRegB;
              append_instr(&head, &tail, add);
              break;
            }
            case ALU_SUB: {
              // Machine: Sub rScratchA, rScratchA, rScratchB
              struct MachineInstr* sub = alloc_machine_instr(MACHINE_SUB);
              sub->ra = kScratchRegA;
              sub->rb = kScratchRegA;
              sub->rc = kScratchRegB;
              append_instr(&head, &tail, sub);
              break;
            }
            case ALU_AND: {
              // Machine: And rScratchA, rScratchA, rScratchB
              struct MachineInstr* and_instr = alloc_machine_instr(MACHINE_AND);
              and_instr->ra = kScratchRegA;
              and_instr->rb = kScratchRegA;
              and_instr->rc = kScratchRegB;
              append_instr(&head, &tail, and_instr);
              break;
            }
            case ALU_OR: {
              // Machine: Or rScratchA, rScratchA, rScratchB
              struct MachineInstr* or_instr = alloc_machine_instr(MACHINE_OR);
              or_instr->ra = kScratchRegA;
              or_instr->rb = kScratchRegA;
              or_instr->rc = kScratchRegB;
              append_instr(&head, &tail, or_instr);
              break;
            }
            case ALU_XOR: {
              // Machine: Xor rScratchA, rScratchA, rScratchB
              struct MachineInstr* xor_instr = alloc_machine_instr(MACHINE_XOR);
              xor_instr->ra = kScratchRegA;
              xor_instr->rb = kScratchRegA;
              xor_instr->rc = kScratchRegB;
              append_instr(&head, &tail, xor_instr);
              break;
            }
            case ALU_SMUL: {
              // Machine: Move args into R1/R2, call smul, move result into scratch A.
              append_builtin_call(&head, &tail, &kBuiltinSmul);
              break;
            }
            case ALU_SDIV: {
              // Machine: Move args into R1/R2, call sdiv, move result into scratch A.
              append_builtin_call(&head, &tail, &kBuiltinSdiv);
              break;
            }
            case ALU_SMOD: {
              // Machine: Move args into R1/R2, call smod, move result into scratch A.
              append_builtin_call(&head, &tail, &kBuiltinSmod);
              break;
            }
            case ALU_UMUL: {
              // Machine: Move args into R1/R2, call umul, move result into scratch A.
              append_builtin_call(&head, &tail, &kBuiltinUmul);
              break;
            }
            case ALU_UDIV: {
              // Machine: Move args into R1/R2, call udiv, move result into scratch A.
              append_builtin_call(&head, &tail, &kBuiltinUdiv);
              break;
            }
            case ALU_UMOD: {
              // Machine: Move args into R1/R2, call umod, move result into scratch A.
              append_builtin_call(&head, &tail, &kBuiltinUmod);
              break;
            }
            case ALU_LSL: {
              // Machine: Move args into R1/R2, call uleft_shift, move result into scratch A.
              append_builtin_call(&head, &tail, &kBuiltinULeftShift);
              break;
            }
            case ALU_LSR: {
              // Machine: Move args into R1/R2, call uright_shift, move result into scratch A.
              append_builtin_call(&head, &tail, &kBuiltinURightShift);
              break;
            }
            case ALU_ASL: {
              // Machine: Move args into R1/R2, call sleft_shift, move result into scratch A.
              append_builtin_call(&head, &tail, &kBuiltinSLeftShift);
              break;
            }
            case ALU_ASR: {
              // Machine: Move args into R1/R2, call sright_shift, move result into scratch A.
              append_builtin_call(&head, &tail, &kBuiltinSRightShift);
              break;
            }
            case ALU_MOV: {
              // Mov rB to rA
              struct MachineInstr* mov = alloc_machine_instr(MACHINE_MOV);
              mov->ra = kScratchRegA;
              mov->rb = kScratchRegB;
              append_instr(&head, &tail, mov);
              break;
            }
            default:
              codegen_error1_int(func_name, cur->type,
                                 "unknown ALU op %d; expected a defined ALU_* variant",
                                 (int)cur->alu_op);
          }
          break;
        case ASM_JUMP: {
          // Machine: Movi rScratchB, label; Br r0, rScratchB
          struct MachineInstr* movi = alloc_machine_instr(MACHINE_MOVI);
          movi->ra = kScratchRegB;
          movi->label = cur->label;
          append_instr(&head, &tail, movi);
          struct MachineInstr* br = alloc_machine_instr(MACHINE_BR);
          br->ra = R0;
          br->rb = kScratchRegB;
          append_instr(&head, &tail, br);
          break;
        }
        case ASM_COND_JUMP: {
          // Expand conditional jump into a short branch-over sequence plus relative jump.
          enum MachineInstrType branch_type = MACHINE_BR;
          switch (cur->cond) {
            case CondE:
              branch_type = MACHINE_BZ;
              break;
            case CondNE:
              branch_type = MACHINE_BNZ;
              break;
            case CondG:
              branch_type = MACHINE_BG;
              break;
            case CondGE:
              branch_type = MACHINE_BGE;
              break;
            case CondL:
              branch_type = MACHINE_BL;
              break;
            case CondLE:
              branch_type = MACHINE_BLE;
              break;
            case CondA:
              branch_type = MACHINE_BA;
              break;
            case CondAE:
              branch_type = MACHINE_BAE;
              break;
            case CondB:
              branch_type = MACHINE_BB;
              break;
            case CondBE:
              branch_type = MACHINE_BBE;
              break;
            default:
              codegen_error1_int(func_name, cur->type,
                                 "unknown condition %d; expected TAC CondE..CondBE",
                                 (int)cur->cond);
          }
          // Machine: B<cond> +4
          struct MachineInstr* cond = alloc_machine_instr(branch_type);
          cond->imm = kCondJumpBranchSkip;
          append_instr(&head, &tail, cond);
          // Machine: Jmp +12
          struct MachineInstr* jmp = alloc_machine_instr(MACHINE_JMP);
          jmp->imm = kCondJumpJmpSkip;
          append_instr(&head, &tail, jmp);
          // Machine: Movi rScratchB, label; Br r0, rScratchB
          struct MachineInstr* movi = alloc_machine_instr(MACHINE_MOVI);
          movi->ra = kScratchRegB;
          movi->label = cur->label;
          append_instr(&head, &tail, movi);
          struct MachineInstr* br = alloc_machine_instr(MACHINE_BR);
          br->ra = R0;
          br->rb = kScratchRegB;
          append_instr(&head, &tail, br);
          break;
        }
        case ASM_LABEL: {
          // Machine: Label
          struct MachineInstr* label = alloc_machine_instr(MACHINE_LABEL);
          label->label = cur->label;
          append_instr(&head, &tail, label);
          break;
        }
        case ASM_CALL: {
          // Machine: Call label
          struct MachineInstr* call = alloc_machine_instr(MACHINE_CALL);
          call->label = cur->label;
          append_instr(&head, &tail, call);
          break;
        }
        case ASM_INDIRECT_CALL: {
          
          struct MachineInstr* call = alloc_machine_instr(MACHINE_BRA);
          call->ra = RA;
          call->rb = kScratchRegA;
          append_instr(&head, &tail, call);
          break;
        }
        case ASM_PUSH: {
          // Machine: Push rScratchA
          struct MachineInstr* push;
          switch (cur->src1->asm_type->type) {
            case BYTE:
              push = alloc_machine_instr(MACHINE_PUSHB);
              break;
            case DOUBLE:
              push = alloc_machine_instr(MACHINE_PUSHD);
              break;
            case WORD:
              push = alloc_machine_instr(MACHINE_PUSH);
              break;
            default:
              codegen_error1_size(func_name, cur->type,
                                  "unsupported push size %zu; expected 1, 2, or 4",
                                  cur->size);
          }
          push->ra = kScratchRegA;
          append_instr(&head, &tail, push);
          break;
        }
        case ASM_RET: {
          // Machine: Comment "Function Epilogue"
          struct MachineInstr* comment = alloc_machine_instr(MACHINE_COMMENT);
          comment->label = &kFunctionEpilogueLabel;
          append_instr(&head, &tail, comment);

          // Machine: Mov sp, bp; Lwa ra, [bp, 4]; Lwa bp, [bp]; Add sp, sp, 8; ret
          struct MachineInstr* mov = alloc_machine_instr(MACHINE_MOV);
          mov->ra = SP;
          mov->rb = BP;
          append_instr(&head, &tail, mov);
          struct MachineInstr* lw_ra = alloc_machine_instr(MACHINE_LWA);
          lw_ra->ra = RA;
          lw_ra->rb = BP;
          lw_ra->imm = kSavedRaOffset;
          append_instr(&head, &tail, lw_ra);
          struct MachineInstr* lw_bp = alloc_machine_instr(MACHINE_LWA);
          lw_bp->ra = BP;
          lw_bp->rb = BP;
          lw_bp->imm = kSavedBpOffset;
          append_instr(&head, &tail, lw_bp);
          struct MachineInstr* addi = alloc_machine_instr(MACHINE_ADD);
          addi->ra = SP;
          addi->rb = SP;
          addi->imm = kEpilogueStackBytes;
          append_instr(&head, &tail, addi);
          struct MachineInstr* ret = alloc_machine_instr(MACHINE_RET);
          append_instr(&head, &tail, ret);
          break;
        }
        case ASM_BOUNDARY: {
          if (cur->loc == NULL) {
            break;
          }
          struct MachineInstr* marker = alloc_machine_instr(MACHINE_DEBUG_LOC);
          marker->debug_loc = cur->loc;
          append_instr(&head, &tail, marker);
          break;
        }
        case ASM_TRUNC: {
          if (cur->size == 1) {
            // Machine: tncb rScratchA, rScratchA
            struct MachineInstr* trunc_instr = alloc_machine_instr(MACHINE_TNCB);
            trunc_instr->ra = kScratchRegA;
            trunc_instr->rb = kScratchRegA;
            append_instr(&head, &tail, trunc_instr);
          } else if (cur->size == 2) {
            // Machine: tncd rScratchA, rScratchA
            struct MachineInstr* trunc_instr = alloc_machine_instr(MACHINE_TNCD);
            trunc_instr->ra = kScratchRegA;
            trunc_instr->rb = kScratchRegA;
            append_instr(&head, &tail, trunc_instr);
          } else {
            codegen_error1_int(func_name, cur->type,
                               "unsupported truncation size %d; expected 1 or 2",
                               (int)cur->size);
          }
          break;
        }
        case ASM_EXTEND: {
          if (cur->size == 1) {
            // Machine: tncb rScratchA, rScratchA
            struct MachineInstr* ext_instr = alloc_machine_instr(MACHINE_SXTB);
            ext_instr->ra = kScratchRegA;
            ext_instr->rb = kScratchRegA;
            append_instr(&head, &tail, ext_instr);
          } else if (cur->size == 2) {
            // Machine: tncd rScratchA, rScratchA
            struct MachineInstr* ext_instr = alloc_machine_instr(MACHINE_SXTD);
            ext_instr->ra = kScratchRegA;
            ext_instr->rb = kScratchRegA;
            append_instr(&head, &tail, ext_instr);
          } else {
            codegen_error1_int(func_name, cur->type,
                               "unsupported extend size %d; expected 1 or 2",
                               (int)cur->size);
          }
          break;
        }
        case ASM_LOAD: {
          // Pointer operand is loaded into rScratchA; copy to rScratchB for the base register.
          struct MachineInstr* mov_ptr = alloc_machine_instr(MACHINE_MOV);
          mov_ptr->ra = kScratchRegB;
          mov_ptr->rb = kScratchRegA;
          append_instr(&head, &tail, mov_ptr);

          struct MachineInstr* load;
          switch (cur->dst->asm_type->type) {
            case BYTE:
              load = alloc_machine_instr(MACHINE_LBA);
              break;
            case DOUBLE:
              load = alloc_machine_instr(MACHINE_LDA);
              break;
            case WORD:
              load = alloc_machine_instr(MACHINE_LWA);
              break;
            default:
              codegen_error1_size(func_name, cur->type,
                                  "unsupported load size %zu; expected 1, 2, or 4",
                                  cur->size);
          }          
          load->ra = kScratchRegA;
          load->rb = kScratchRegB;
          load->imm = 0;
          append_instr(&head, &tail, load);
          break;
        }
        case ASM_STORE: {
          struct MachineInstr* store;
          switch (cur->src1->asm_type->type) {
            case BYTE:
              store = alloc_machine_instr(MACHINE_SBA);
              break;
            case DOUBLE:
              store = alloc_machine_instr(MACHINE_SDA);
              break;
            case WORD:
              store = alloc_machine_instr(MACHINE_SWA);
              break;
            default:
              codegen_error1_size(func_name, cur->type,
                                  "unsupported store size %zu; expected 1, 2, or 4",
                                  cur->size);
          }          
          store->ra = kScratchRegA;
          store->rb = kScratchRegB;
          store->imm = 0;
          append_instr(&head, &tail, store);
          break;
        }
        case ASM_GET_ADDRESS:
          codegen_error0(func_name, cur->type,
                         "unsupported GetAddress operands; expected dst=Memory and src=Memory or Data");
        default:
          codegen_error1_int(func_name, cur->type,
                             "unknown ASM instruction type %d", (int)cur->type);
      }

      struct Operand* dst = get_dst(cur);
      if (dst != NULL) {
        if (dst->type == OPERAND_REG) {
          // Machine: Mov dst, rScratchA
          struct MachineInstr* mov = alloc_machine_instr(MACHINE_MOV);
          mov->ra = dst->reg;
          mov->rb = kScratchRegA;
          append_instr(&head, &tail, mov);
        } else if (dst->type == OPERAND_MEMORY) {
          // Machine: Swa rScratchA, [rBase, off]
          struct MachineInstr* store;
          switch (dst->asm_type->type) {
            case BYTE:
              store = alloc_machine_instr(MACHINE_SBA);
              break;
            case DOUBLE:
              store = alloc_machine_instr(MACHINE_SDA);
              break;
            case WORD:
              store = alloc_machine_instr(MACHINE_SWA);
              break;
            default:
              codegen_error1_int(func_name, cur->type,
                                 "unsupported asm type %d for destination operand",
                                 (int)dst->asm_type->type);
              break;
          }
          store->ra = kScratchRegA;
          store->rb = dst->reg;
          store->imm = dst->lit_value;
          append_instr(&head, &tail, store);
        } else if (dst->type == OPERAND_DATA) {
          if (dst->lit_value != 0) {
            // Preserve scratch A while computing address for label+offset.
            struct MachineInstr* push = alloc_machine_instr(MACHINE_PUSH);
            push->ra = kScratchRegA;
            append_instr(&head, &tail, push);

            emit_label_address(&head, &tail, kScratchRegB, kScratchRegA,
                               dst->pseudo, dst->lit_value);

            struct MachineInstr* pop = alloc_machine_instr(MACHINE_POP);
            pop->ra = kScratchRegA;
            append_instr(&head, &tail, pop);

            struct MachineInstr* store;
            switch (dst->asm_type->type) {
              case BYTE:
                store = alloc_machine_instr(MACHINE_SBA);
                break;
              case DOUBLE:
                store = alloc_machine_instr(MACHINE_SDA);
                break;
              case WORD:
                store = alloc_machine_instr(MACHINE_SWA);
                break;
              default:
                codegen_error1_int(func_name, cur->type,
                                   "unsupported asm type %d for destination operand",
                                   (int)dst->asm_type->type);
                break;
            }
            store->ra = kScratchRegA;
            store->rb = kScratchRegB;
            store->imm = 0;
            append_instr(&head, &tail, store);
          } else {
            // Machine: Sw rScratchA, [label]
            struct MachineInstr* store;
            switch (dst->asm_type->type) {
              case BYTE:
                store = alloc_machine_instr(MACHINE_SB);
                break;
              case DOUBLE:
                store = alloc_machine_instr(MACHINE_SD);
                break;
              case WORD:
                store = alloc_machine_instr(MACHINE_SW);
                break;
              default:
                codegen_error1_int(func_name, cur->type,
                                   "unsupported asm type %d for destination operand",
                                   (int)dst->asm_type->type);
                break;
            }
            store->ra = kScratchRegA;
            store->rb = R0;
            store->imm = kZeroOffset;
            store->label = dst->pseudo;
            append_instr(&head, &tail, store);
          }
        } else {
          codegen_error1_int(func_name, cur->type,
                             "invalid destination operand type %d; expected Reg, Memory, or Data",
                             (int)dst->type);
        }
      }
    }

    if (head != NULL) {
      if (machine_prog->head == NULL) {
        machine_prog->head = head;
        machine_prog->tail = tail;
      } else {
        machine_prog->tail->next = head;
        machine_prog->tail = tail;
      }
    }
  }

  return machine_prog;
}

struct MachineProg* top_level_to_machine(struct AsmTopLevel* asm_top){
  struct MachineProg* machine_prog = arena_alloc(sizeof(struct MachineProg));
  machine_prog->head = NULL;
  machine_prog->tail = NULL;

  if (asm_top->type == ASM_FUNC){
    //  .global func         # optional global label
    //  func:
    //    # function prologue
    //    push ra            # save return address
    //    push bp            # save base pointer
    //    mov  bp, sp        # set base pointer to current stack pointer
    //    # function body

    struct MachineInstr* newline = arena_alloc(sizeof(struct MachineInstr));
    newline->type = MACHINE_NEWLINE;

    struct MachineInstr* label = arena_alloc(sizeof(struct MachineInstr));
    if (asm_top->global) {
      // Machine: Label <func>
      struct MachineInstr* global = arena_alloc(sizeof(struct MachineInstr));
      global->type = MACHINE_GLOBAL;
      global->label = asm_top->name;
      global->next = label;
      newline->next = global;
    } else {
      newline->next = label;
    }

    // Machine: Label <func>
    label->type = MACHINE_LABEL;
    label->label = asm_top->name;

    char* entry_loc = find_function_entry_loc(asm_top->body);

    // Machine: Comment "Function Prologue"
    struct MachineInstr* prologue_comment = arena_alloc(sizeof(struct MachineInstr));
    prologue_comment->type = MACHINE_COMMENT;
    prologue_comment->label = &kFunctionPrologueLabel;
    if (entry_loc != NULL) {
      // Emit a line marker at function entry so debugger locations are valid at the label.
      struct MachineInstr* entry_marker = alloc_machine_instr(MACHINE_DEBUG_LOC);
      entry_marker->debug_loc = entry_loc;
      label->next = entry_marker;
      entry_marker->next = prologue_comment;
    } else {
      label->next = prologue_comment;
    }
    
    // Machine: Push ra
    struct MachineInstr* push_ra = arena_alloc(sizeof(struct MachineInstr));
    push_ra->type = MACHINE_PUSH;
    push_ra->ra = RA;
    prologue_comment->next = push_ra;

    // Machine: Push bp
    struct MachineInstr* push_bp = arena_alloc(sizeof(struct MachineInstr));
    push_bp->type = MACHINE_PUSH;
    push_bp->ra = BP;
    push_ra->next = push_bp;

    // Machine: Mov bp, sp
    struct MachineInstr* set_bp = arena_alloc(sizeof(struct MachineInstr));
    set_bp->type = MACHINE_MOV;
    set_bp->ra = BP;
    set_bp->rb = SP;
    push_bp->next = set_bp;
    set_bp->next = NULL;

    // Emit stack layout comments for user-visible locals (if debug markers exist).
    struct MachineInstr* locals_tail = set_bp;
    if (asm_top->locals != NULL) {
      for (struct DebugLocal* local = asm_top->locals; local != NULL; local = local->next) {
        struct MachineInstr* local_instr = alloc_machine_instr(MACHINE_DEBUG_LOCAL);
        local_instr->debug_name = local->name;
        local_instr->debug_offset = local->offset;
        local_instr->imm = local->size;
        locals_tail->next = local_instr;
        locals_tail = local_instr;
      }
    }

    // Machine: Comment "Function Body"
    struct MachineInstr* body_comment = arena_alloc(sizeof(struct MachineInstr));
    body_comment->type = MACHINE_COMMENT;
    body_comment->label = &kFunctionBodyLabel;
    locals_tail->next = body_comment;

    // append prologue to machine_prog
    machine_prog->head = newline;
    machine_prog->tail = body_comment;

    // function body
    struct MachineProg* body_instrs = instr_to_machine(asm_top->name, asm_top->body);
    machine_prog->tail->next = body_instrs->head;
    machine_prog->tail = body_instrs->tail;

  } else if (asm_top->type == ASM_STATIC_VAR || asm_top->type == ASM_STATIC_CONST){
    struct AsmSymbolEntry* sym_entry = asm_symbol_table_get(asm_symbol_table, asm_top->name);
    if (sym_entry == NULL) {
      codegen_error1_slice(NULL, 0,
                           "Undefined symbol '%.*s' in codegen",
                           asm_top->name);
    }
    struct AsmType* type = sym_entry->type;

    // emit .align directive
    struct MachineInstr* align_instr = arena_alloc(sizeof(struct MachineInstr));
    align_instr->type = MACHINE_ALIGN;
    align_instr->imm = asm_type_alignment(type);
    machine_prog->head = align_instr;
    machine_prog->tail = align_instr;

    if (asm_top->global) {
      // Machine: .global var
      struct MachineInstr* global = arena_alloc(sizeof(struct MachineInstr));
      global->type = MACHINE_GLOBAL;
      global->label = asm_top->name;

      // append global to machine_prog
      machine_prog->tail->next = global;
      machine_prog->tail = global;
    }

    // static variable
    struct InitList* init = asm_top->init_values;
    struct MachineInstr* data_label = alloc_machine_instr(MACHINE_LABEL);
    data_label->label = asm_top->name;
    machine_prog->tail->next = data_label;
    machine_prog->tail = data_label;

    // Machine: .space or .fill for static data
    struct MachineInstr* data_instr = make_data(init, type);

    // append data instructions to machine_prog
    struct MachineInstr* data_tail = data_instr;
    while (data_tail->next != NULL) {
      data_tail = data_tail->next;
    }
    machine_prog->tail->next = data_instr;
    machine_prog->tail = data_tail;
  } else if (asm_top->type == ASM_SECTION){
    // directive
    struct MachineInstr* dir_instr = arena_alloc(sizeof(struct MachineInstr));
    dir_instr->type = MACHINE_SECTION;
    dir_instr->label = asm_top->name;

    // append data_instr to machine_prog
    machine_prog->head = dir_instr;
    machine_prog->tail = dir_instr;
  } else if (asm_top->type == ASM_ALIGN) {
    // directive
    struct MachineInstr* align_instr = arena_alloc(sizeof(struct MachineInstr));
    align_instr->type = MACHINE_ALIGN;
    align_instr->imm = asm_top->alignment;

    // append data_instr to machine_prog
    machine_prog->head = align_instr;
    machine_prog->tail = align_instr;
  } else {
    // Error
    codegen_error0(NULL, 0, "Unknown AsmTopLevelType in codegen");
  }

  return machine_prog;
}

static struct MachineInstr* make_data(struct InitList* init, struct AsmType* type){
  if (init == NULL) {
    // Tentative definitions emit zero-filled storage for the full symbol size.
    struct MachineInstr* instr = alloc_machine_instr(MACHINE_SPACE);
    instr->imm = (int)asm_type_size(type);
    return instr;
  }

  struct MachineInstr* instr = NULL;
  struct MachineInstr* tail = NULL;
  for (struct InitList* cur = init; cur != NULL; cur = cur->next){
    struct MachineInstr* cur_instr = alloc_machine_instr(MACHINE_FILL);

    bool was_string = false;

    switch (cur->value->int_type) {
      case CHAR_INIT:
      case UCHAR_INIT:
        cur_instr->type = MACHINE_FILB;
        break;
      case SHORT_INIT:
      case USHORT_INIT:
        cur_instr->type = MACHINE_FILD;
        break;
      case INT_INIT:
      case UINT_INIT:
        cur_instr->type = MACHINE_FILL;
        break;
      case POINTER_INIT:
        cur_instr->type = MACHINE_FILL;
        cur_instr->label = cur->value->value.pointer;
        break;
      case LONG_INIT:
      case ULONG_INIT:
        {
          uint64_t raw = cur->value->value.num;
          uint32_t low = (uint32_t)(raw & 0xFFFFFFFFu);
          uint32_t high = (uint32_t)((raw >> 32) & 0xFFFFFFFFu);

          struct MachineInstr* low_instr = alloc_machine_instr(MACHINE_FILL);
          low_instr->imm = (int32_t)low;

          struct MachineInstr* high_instr = alloc_machine_instr(MACHINE_FILL);
          high_instr->imm = (int32_t)high;

          if (instr == NULL) {
            instr = low_instr;
            tail = low_instr;
          } else {
            tail->next = low_instr;
            tail = low_instr;
          }
          tail->next = high_instr;
          tail = high_instr;
          was_string = true;
          break;
        }
      case ZERO_INIT:
        cur_instr->type = MACHINE_SPACE;
        break;
      case STRING_INIT:
        was_string = true;
        // emit sequence of FILB instructions for each character in the string
        // padding/null bytes are emitted explicitly via ZERO_INIT nodes
        for (size_t i = 0; i < cur->value->value.string->len; i++) {
          struct MachineInstr* char_instr = alloc_machine_instr(MACHINE_FILB);
          char_instr->imm = (int)(unsigned char)cur->value->value.string->start[i];
          if (instr == NULL){
            instr = char_instr;
            tail = char_instr;
          } else {
            tail->next = char_instr;
            tail = char_instr;
          }
        }

        break;
      default:
        codegen_error1_int(NULL, 0,
                           "unsupported static initializer type %d",
                           (int)cur->value->int_type);
    }

    if (!was_string){
      // set value and append to list (string does this on its own)
      if (cur_instr->label == NULL) {
        cur_instr->imm = (int)cur->value->value.num;
      }

      if (instr == NULL){
        instr = cur_instr;
        tail = cur_instr;
      } else {
        tail->next = cur_instr;
        tail = cur_instr;
      }
    }
  }

  return instr;
}

struct MachineProg* prog_to_machine(struct AsmProg* asm_prog){
  struct MachineProg* machine_prog = arena_alloc(sizeof(struct MachineProg));
  machine_prog->head = NULL;
  machine_prog->tail = NULL;

  for (struct AsmTopLevel* top = asm_prog->head; top != NULL; top = top->next){
    struct MachineProg* top_instrs = top_level_to_machine(top);
    if (machine_prog->head == NULL){
      machine_prog->head = top_instrs->head;
      machine_prog->tail = top_instrs->tail;
    } else {
      machine_prog->tail->next = top_instrs->head;
      machine_prog->tail = top_instrs->tail;
    }
  }

  return machine_prog;
}
