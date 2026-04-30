#include "asm_gen.h"
#include "arena.h"
#include "typechecking.h"
#include "unique_name.h"

#include "../crt/stdio.h"
#include "../crt/print.h"
#include "../crt/stdlib.h"
#include "../crt/stddef.h"
#include "../crt/assert.h"

struct PseudoMap* pseudo_map = NULL;

static struct Slice text_directive_slice = {"text", 4};
static struct Slice data_directive_slice = {"data", 4};

// Use caller-saved registers that are not argument registers for scratch work.
enum Reg kScratchRegA = R9;
enum Reg kScratchRegB = R10;
enum Reg kScratchRegC = R11;

static size_t REG_ARG_LIMIT = 8;
static size_t kStackSlotBytes = 4;

static char kTempMarker[6] = ".tmp.";
static size_t kTempMarkerLen = sizeof(kTempMarker) - 1;

struct AsmSymbolTable* asm_symbol_table = NULL;

static struct AsmType kByteType = { BYTE, {0, 0} };
static struct AsmType kDoubleType = { DOUBLE, {0, 0} };
static struct AsmType kWordType = { WORD, {0, 0} };
static struct AsmType kLongWordType = { LONG_WORD, {0, 0} };

static void asm_gen_error0(char* operation, struct Slice* func_name, char* message);
static void asm_gen_error1_int(char* operation, struct Slice* func_name, char* fmt, int arg0);
static void asm_gen_error1_size(char* operation, struct Slice* func_name, char* fmt, size_t arg0);
static void asm_gen_error1_slice(char* operation, struct Slice* func_name, char* fmt,
                                 struct Slice* slice);
static void asm_gen_error1_len_str(char* operation, struct Slice* func_name, char* fmt,
                                   int len, char* str);
static void asm_gen_error2_size(char* operation, struct Slice* func_name, char* fmt,
                                size_t arg0, size_t arg1);
static void asm_gen_error2_int_str(char* operation, struct Slice* func_name, char* fmt,
                                   int arg0, char* arg1);

static struct Operand kStackMem;

static struct Operand* stack_mem_operand(void) {
  kStackMem.type = OPERAND_MEMORY;
  kStackMem.asm_type = &kWordType;
  kStackMem.reg = SP;
  kStackMem.lit_value = 0;
  kStackMem.pseudo = NULL;
  return &kStackMem;
}

// Purpose: Identify aggregate types that require byte-wise copies.
// Inputs: type is the source/target type.
// Outputs: Returns true for arrays, structs, and unions.
// Invariants/Assumptions: type is non-NULL when called.
static bool is_aggregate_type(struct Type* type) {
  if (type == NULL) {
    return false;
  }
  return type->type == ARRAY_TYPE ||
         type->type == STRUCT_TYPE ||
         type->type == UNION_TYPE;
}

// Purpose: Determine the base alignment for an operand's backing storage.
// Inputs: opr is the operand to inspect.
// Outputs: Returns the alignment in bytes.
// Invariants/Assumptions: asm_symbol_table is initialized before use.
static size_t operand_base_alignment(struct Operand* opr) {
  if (opr == NULL || opr->asm_type == NULL) {
    return 1;
  }

  size_t alignment = asm_type_alignment(opr->asm_type);
  if ((opr->type == OPERAND_PSEUDO ||
       opr->type == OPERAND_PSEUDO_MEM ||
       opr->type == OPERAND_DATA) &&
      opr->pseudo != NULL &&
      asm_symbol_table != NULL) {
    struct AsmSymbolEntry* sym_entry = asm_symbol_table_get(asm_symbol_table, opr->pseudo);
    if (sym_entry != NULL && sym_entry->type != NULL) {
      alignment = asm_type_alignment(sym_entry->type);
    }
  }

  return alignment;
}

struct AsmType* type_to_asm_type(struct Type* type){
  switch (type->type){
    case CHAR_TYPE:
    case SCHAR_TYPE:
    case UCHAR_TYPE:
      return &kByteType;
    case SHORT_TYPE:
    case USHORT_TYPE:
      return &kDoubleType;
    case INT_TYPE:
    case UINT_TYPE:
    case ENUM_TYPE:
    case POINTER_TYPE:
      return &kWordType;
    case LONG_TYPE:
    case ULONG_TYPE:
      return &kLongWordType;
    case ARRAY_TYPE: 
    case STRUCT_TYPE:
    case UNION_TYPE: {
      struct AsmType* asm_type = arena_alloc(sizeof(struct AsmType));
      asm_type->type = BYTE_ARRAY;
      asm_type->byte_array.size = get_type_size(type);
      asm_type->byte_array.alignment = type_alignment(type, NULL);
      return asm_type;
    }
    case FUN_TYPE:
      return NULL; // dont error, functions just have no asm type
    default:
      // unknown type
      asm_gen_error1_int("symbol table", NULL,
                         "invalid type %d for ASM symbol conversion", type->type);
      return NULL;
  }
}

// Purpose: Create a fresh pseudo temp for byte-copy ops and register it in the ASM symbol table.
// Inputs: func_name identifies the owning function; asm_type selects the temp size.
// Outputs: Returns an OPERAND_PSEUDO operand with a unique temp name.
// Invariants/Assumptions: asm_symbol_table is initialized before use.
static struct Operand* make_asm_temp(struct Slice* func_name, struct AsmType* asm_type) {
  if (asm_symbol_table == NULL) {
    asm_gen_error0("copy-bytes", func_name,
                   "asm symbol table not initialized before temp creation");
  }

  struct Operand* temp = arena_alloc(sizeof(struct Operand));
  temp->type = OPERAND_PSEUDO;
  temp->pseudo = make_unique_label(func_name, "tmp.asm");
  temp->asm_type = asm_type;

  asm_symbol_table_insert(asm_symbol_table, temp->pseudo, asm_type, false, false, false);
  return temp;
}

// Purpose: Append an ASM instruction node to a list.
// Inputs: head/tail are the list pointers; node is the instruction to append.
// Outputs: Updates head/tail to include node at the end.
// Invariants/Assumptions: node->next is either NULL or a valid list tail.
static void append_asm_instr(struct AsmInstr** head, struct AsmInstr** tail, struct AsmInstr* node) {
  if (head == NULL || tail == NULL || node == NULL) {
    asm_gen_error0("asm-instr", NULL, "append requested with NULL list or node");
  }

  if (*head == NULL) {
    *head = node;
    *tail = node;
    return;
  }

  (*tail)->next = node;
  *tail = node;
}

// Purpose: Append a list of ASM instructions to an existing list.
// Inputs: head/tail are the destination list; list is the head of the list to append.
// Outputs: Updates head/tail to include list at the end.
// Invariants/Assumptions: list is either NULL or a well-formed list.
static void append_asm_instrs(struct AsmInstr** head, struct AsmInstr** tail, struct AsmInstr* list) {
  if (list == NULL) {
    return;
  }
  if (head == NULL || tail == NULL) {
    asm_gen_error0("asm-instr", NULL, "append list requested with NULL list pointers");
  }
  if (*head == NULL) {
    *head = list;
  } else {
    (*tail)->next = list;
  }
  while (list->next != NULL) {
    list = list->next;
  }
  *tail = list;
}

// Purpose: Return the length of an operand list.
// Inputs: list is the operand list head (may be NULL).
// Outputs: Returns the number of nodes.
// Invariants/Assumptions: list is acyclic.
static size_t operand_list_length(struct OperandList* list) {
  size_t count = 0;
  for (struct OperandList* cur = list; cur != NULL; cur = cur->next) {
    count++;
  }
  return count;
}

// Purpose: Fetch an operand from a list by index.
// Inputs: list is the operand list head; index is zero-based.
// Outputs: Returns the operand pointer at index or errors if out of range.
// Invariants/Assumptions: list is acyclic.
static struct Operand* operand_list_get(struct OperandList* list, size_t index) {
  struct OperandList* cur = list;
  for (size_t i = 0; i < index; i++) {
    if (cur == NULL) {
      asm_gen_error1_size("operand-list", NULL,
                          "operand list index %zu out of range", index);
    }
    cur = cur->next;
  }
  if (cur == NULL || cur->opr == NULL) {
    asm_gen_error1_size("operand-list", NULL,
                        "operand list index %zu out of range", index);
  }
  return cur->opr;
}

// Purpose: Build a direct memory operand.
// Inputs: base is the base register; offset is the byte offset; asm_type selects access size.
// Outputs: Returns a new OPERAND_MEMORY operand.
// Invariants/Assumptions: asm_type is non-NULL.
static struct Operand* make_asm_mem(enum Reg base, int offset, struct AsmType* asm_type) {
  if (asm_type == NULL) {
    asm_gen_error0("operand", NULL, "NULL asm type for memory operand");
  }
  struct Operand* opr = arena_alloc(sizeof(struct Operand));
  opr->type = OPERAND_MEMORY;
  opr->reg = base;
  opr->lit_value = offset;
  opr->asm_type = asm_type;
  opr->pseudo = NULL;
  return opr;
}

// Purpose: Create an operand representing base+offset for byte/word accesses.
// Inputs: base is the base operand; offset is the byte delta; asm_type selects access size.
// Outputs: Returns a new operand with adjusted offset.
// Invariants/Assumptions: base is not NULL; data operands do not support non-zero offsets.
static struct Operand* add_offset_typed(struct Operand* base, int offset, struct AsmType* asm_type) {
  if (base == NULL) {
    asm_gen_error0("operand", NULL, "NULL base operand for offset");
  }
  switch (base->type) {
    case OPERAND_PSEUDO_MEM:
      return make_pseudo_mem(base->pseudo, asm_type, base->lit_value + offset);
    case OPERAND_PSEUDO:
      return make_pseudo_mem(base->pseudo, asm_type, offset);
    case OPERAND_MEMORY:
      return make_asm_mem(base->reg, base->lit_value + offset, asm_type);
    case OPERAND_DATA:
      {
        struct Operand* opr = arena_alloc(sizeof(struct Operand));
        opr->type = OPERAND_DATA;
        opr->pseudo = base->pseudo;
        opr->asm_type = asm_type;
        opr->lit_value = base->lit_value + offset;
        return opr;
      }
    case OPERAND_REG:
      if (offset != 0) {
        asm_gen_error0("operand", NULL, "register operand cannot take offset");
      }
      {
        struct Operand* opr = arena_alloc(sizeof(struct Operand));
        *opr = *base;
        opr->asm_type = asm_type;
        return opr;
      }
    default:
      asm_gen_error1_int("operand", NULL,
                         "unsupported operand type %d for offset", (int)base->type);
      return NULL;
  }
}

// Purpose: Create a byte-sized operand at base+offset.
// Inputs: base is the base operand; offset is the byte delta.
// Outputs: Returns a byte-typed operand.
// Invariants/Assumptions: base is not NULL.
// Purpose: Check whether a symbol name refers to a static or constant storage symbol.
// Inputs: name is the symbol identifier.
// Outputs: Returns true if the symbol is static/or a function.
// Invariants/Assumptions: global_symbol_table is initialized before use.
static bool is_static_symbol_name(struct Slice* name) {
  if (name == NULL || global_symbol_table == NULL) {
    return false;
  }
  struct SymbolEntry* entry = symbol_table_get(global_symbol_table, name);
  if (entry == NULL || entry->attrs == NULL) {
    return false;
  }
  if (entry->type != NULL && entry->type->type == FUN_TYPE) {
    return true;
  }
  return entry->attrs->attr_type == STATIC_ATTR ||
         entry->attrs->attr_type == CONST_ATTR;
}

// Purpose: Forward declaration for aggregate classification helpers.
// Inputs/Outputs: See definition below.
// Invariants/Assumptions: Declared early for use in symbol conversion.
static struct VarClassList* classify_struct(struct StructEntry* struct_entry);

struct AsmInstr* copy_bytes(struct Slice* func_name, struct Operand* src, struct Operand* dst, size_t size){
  struct AsmInstr* head = NULL;
  struct AsmInstr* tail = NULL;
  
  size_t src_alignment = operand_base_alignment(src);
  size_t dst_alignment = operand_base_alignment(dst);
  size_t kWordBytes = 4;
  size_t kDoubleBytes = 2;
  size_t kByteBytes = 1;

  if (src == NULL || dst == NULL) {
    asm_gen_error0("copy-bytes", func_name, "NULL operand for byte copy");
  }

  for (size_t offset = 0; offset < size; ){
    size_t remaining = size - offset;
    size_t chunk = kByteBytes;

    if (remaining >= kWordBytes &&
        ((int)offset % (int)kWordBytes) == 0 &&
        src_alignment >= kWordBytes &&
        dst_alignment >= kWordBytes) {
      chunk = kWordBytes;
    } else if (remaining >= kDoubleBytes &&
               ((int)offset % (int)kDoubleBytes) == 0 &&
               src_alignment >= kDoubleBytes &&
               dst_alignment >= kDoubleBytes) {
      chunk = kDoubleBytes;
    }

    struct AsmType* chunk_type = &kByteType;
    if (chunk == kWordBytes) {
      chunk_type = &kWordType;
    } else if (chunk == kDoubleBytes) {
      chunk_type = &kDoubleType;
    }

    struct Operand* src_op = add_offset_typed(src, (int)offset, chunk_type);
    struct Operand* dst_op = add_offset_typed(dst, (int)offset, chunk_type);
    struct AsmInstr* mov = arena_alloc(sizeof(struct AsmInstr));
    mov->type = ASM_MOV;
    mov->dst = dst_op;
    mov->src1 = src_op;
    mov->src2 = NULL;
    mov->next = NULL;
    append_asm_instr(&head, &tail, mov);

    offset += chunk;
  }
  
  return head;
}

struct AsmInstr* copy_bytes_to_reg(struct Slice* func_name, struct Operand* src, enum Reg dst_reg, size_t size){
  if (size == 0) {
    return NULL;
  }

  size_t src_alignment = operand_base_alignment(src);
  bool allow_word = src_alignment >= 4;
  bool allow_double = src_alignment >= 2;

  if ((size == 4 && !allow_word) || (size == 2 && !allow_double) || (size == 3 && !allow_double)) {
    struct AsmInstr* head = NULL;
    struct AsmInstr* tail = NULL;

    struct Operand* temp = make_asm_temp(func_name, &kWordType);
    struct AsmInstr* zero = arena_alloc(sizeof(struct AsmInstr));
    zero->type = ASM_MOV;
    zero->dst = temp;
    zero->src1 = arena_alloc(sizeof(struct Operand));
    zero->src1->type = OPERAND_LIT;
    zero->src1->lit_value = 0;
    zero->src1->asm_type = &kWordType;
    zero->src2 = NULL;
    zero->next = NULL;
    append_asm_instr(&head, &tail, zero);

    struct AsmInstr* copy_instrs = copy_bytes(func_name, src, temp, size);
    append_asm_instrs(&head, &tail, copy_instrs);

    struct AsmInstr* mov = arena_alloc(sizeof(struct AsmInstr));
    mov->type = ASM_MOV;
    mov->dst = arena_alloc(sizeof(struct Operand));
    mov->dst->type = OPERAND_REG;
    mov->dst->reg = dst_reg;
    mov->dst->asm_type = &kWordType;
    mov->src1 = temp;
    mov->src2 = NULL;
    mov->next = NULL;
    append_asm_instr(&head, &tail, mov);

    return head;
  }

  struct AsmType* chunk_type = NULL;
  if (size == 4) {
    chunk_type = &kWordType;
  } else if (size == 2) {
    chunk_type = &kDoubleType;
  } else if (size == 1) {
    chunk_type = &kByteType;
  } else if (size == 3) {
    struct AsmInstr* head = NULL;
    struct AsmInstr* tail = NULL;

    struct Operand* dst_reg_op = arena_alloc(sizeof(struct Operand));
    dst_reg_op->type = OPERAND_REG;
    dst_reg_op->reg = dst_reg;
    dst_reg_op->asm_type = &kWordType;

    struct AsmInstr* load_low = arena_alloc(sizeof(struct AsmInstr));
    load_low->type = ASM_MOV;
    load_low->dst = dst_reg_op;
    load_low->src1 = add_offset_typed(src, 0, &kDoubleType);
    load_low->src2 = NULL;
    load_low->next = NULL;
    append_asm_instr(&head, &tail, load_low);

    // Mask to avoid sign-extension artifacts from 16-bit loads when packing 3 bytes.
    struct Operand* low_mask = arena_alloc(sizeof(struct Operand));
    low_mask->type = OPERAND_LIT;
    low_mask->lit_value = 0xFFFF;
    low_mask->asm_type = &kWordType;

    struct AsmInstr* mask_low = arena_alloc(sizeof(struct AsmInstr));
    mask_low->type = ASM_BINARY;
    mask_low->alu_op = ALU_AND;
    mask_low->dst = dst_reg_op;
    mask_low->src1 = dst_reg_op;
    mask_low->src2 = low_mask;
    mask_low->next = NULL;
    append_asm_instr(&head, &tail, mask_low);

    struct Operand* tmp_reg = arena_alloc(sizeof(struct Operand));
    tmp_reg->type = OPERAND_REG;
    tmp_reg->reg = kScratchRegA;
    tmp_reg->asm_type = &kWordType;

    struct AsmInstr* load_high = arena_alloc(sizeof(struct AsmInstr));
    load_high->type = ASM_MOV;
    load_high->dst = tmp_reg;
    load_high->src1 = add_offset_typed(src, 2, &kByteType);
    load_high->src2 = NULL;
    load_high->next = NULL;
    append_asm_instr(&head, &tail, load_high);

    // Mask the high byte to avoid sign-extension artifacts from byte loads.
    struct Operand* high_mask = arena_alloc(sizeof(struct Operand));
    high_mask->type = OPERAND_LIT;
    high_mask->lit_value = 0xFF;
    high_mask->asm_type = &kWordType;

    struct AsmInstr* mask_high = arena_alloc(sizeof(struct AsmInstr));
    mask_high->type = ASM_BINARY;
    mask_high->alu_op = ALU_AND;
    mask_high->dst = tmp_reg;
    mask_high->src1 = tmp_reg;
    mask_high->src2 = high_mask;
    mask_high->next = NULL;
    append_asm_instr(&head, &tail, mask_high);

    struct Operand* shift_amount = arena_alloc(sizeof(struct Operand));
    shift_amount->type = OPERAND_LIT;
    shift_amount->lit_value = 16;
    shift_amount->asm_type = &kWordType;

    struct AsmInstr* shift = arena_alloc(sizeof(struct AsmInstr));
    shift->type = ASM_BINARY;
    shift->alu_op = ALU_LSL;
    shift->dst = tmp_reg;
    shift->src1 = tmp_reg;
    shift->src2 = shift_amount;
    shift->next = NULL;
    append_asm_instr(&head, &tail, shift);

    struct AsmInstr* or_instr = arena_alloc(sizeof(struct AsmInstr));
    or_instr->type = ASM_BINARY;
    or_instr->alu_op = ALU_OR;
    or_instr->dst = dst_reg_op;
    or_instr->src1 = dst_reg_op;
    or_instr->src2 = tmp_reg;
    or_instr->next = NULL;
    append_asm_instr(&head, &tail, or_instr);

    return head;
  } else {
    asm_gen_error1_size("copy-bytes", func_name,
                        "unsupported register copy size %zu; expected 1, 2, or 4",
                        size);
  }

  struct AsmInstr* mov = arena_alloc(sizeof(struct AsmInstr));
  mov->type = ASM_MOV;
  mov->dst = arena_alloc(sizeof(struct Operand));
  mov->dst->type = OPERAND_REG;
  mov->dst->reg = dst_reg;
  mov->dst->asm_type = chunk_type;
  mov->src1 = add_offset_typed(src, 0, chunk_type);
  mov->src2 = NULL;
  mov->next = NULL;
  return mov;
} 

struct AsmInstr* copy_bytes_from_reg(struct Slice* func_name, enum Reg src_reg, struct Operand* dst, size_t size){
  if (size == 0) {
    return NULL;
  }

  size_t dst_alignment = operand_base_alignment(dst);
  bool allow_word = dst_alignment >= 4;
  bool allow_double = dst_alignment >= 2;

  if ((size == 4 && !allow_word) || (size == 2 && !allow_double) || (size == 3 && !allow_double)) {
    struct AsmInstr* head = NULL;
    struct AsmInstr* tail = NULL;

    struct Operand* temp = make_asm_temp(func_name, &kWordType);
    struct AsmInstr* mov = arena_alloc(sizeof(struct AsmInstr));
    mov->type = ASM_MOV;
    mov->dst = temp;
    mov->src1 = arena_alloc(sizeof(struct Operand));
    mov->src1->type = OPERAND_REG;
    mov->src1->reg = src_reg;
    mov->src1->asm_type = &kWordType;
    mov->src2 = NULL;
    mov->next = NULL;
    append_asm_instr(&head, &tail, mov);

    struct AsmInstr* copy_instrs = copy_bytes(func_name, temp, dst, size);
    append_asm_instrs(&head, &tail, copy_instrs);

    return head;
  }

  struct AsmType* chunk_type = NULL;
  if (size == 4) {
    chunk_type = &kWordType;
  } else if (size == 2) {
    chunk_type = &kDoubleType;
  } else if (size == 1) {
    chunk_type = &kByteType;
  } else if (size == 3) {
    struct AsmInstr* head = NULL;
    struct AsmInstr* tail = NULL;

    struct Operand* src_reg_op = arena_alloc(sizeof(struct Operand));
    src_reg_op->type = OPERAND_REG;
    src_reg_op->reg = src_reg;
    src_reg_op->asm_type = &kWordType;

    struct AsmInstr* store_low = arena_alloc(sizeof(struct AsmInstr));
    store_low->type = ASM_MOV;
    store_low->dst = add_offset_typed(dst, 0, &kDoubleType);
    store_low->src1 = src_reg_op;
    store_low->src2 = NULL;
    store_low->next = NULL;
    append_asm_instr(&head, &tail, store_low);

    struct Operand* tmp_reg = arena_alloc(sizeof(struct Operand));
    tmp_reg->type = OPERAND_REG;
    tmp_reg->reg = kScratchRegA;
    tmp_reg->asm_type = &kWordType;

    struct AsmInstr* mov_tmp = arena_alloc(sizeof(struct AsmInstr));
    mov_tmp->type = ASM_MOV;
    mov_tmp->dst = tmp_reg;
    mov_tmp->src1 = src_reg_op;
    mov_tmp->src2 = NULL;
    mov_tmp->next = NULL;
    append_asm_instr(&head, &tail, mov_tmp);

    // Clear upper bits before extracting the high byte to avoid sign-extension artifacts.
    struct Operand* mid_mask = arena_alloc(sizeof(struct Operand));
    mid_mask->type = OPERAND_LIT;
    mid_mask->lit_value = 0x00FFFFFF;
    mid_mask->asm_type = &kWordType;

    struct AsmInstr* mask_mid = arena_alloc(sizeof(struct AsmInstr));
    mask_mid->type = ASM_BINARY;
    mask_mid->alu_op = ALU_AND;
    mask_mid->dst = tmp_reg;
    mask_mid->src1 = tmp_reg;
    mask_mid->src2 = mid_mask;
    mask_mid->next = NULL;
    append_asm_instr(&head, &tail, mask_mid);

    struct Operand* shift_amount = arena_alloc(sizeof(struct Operand));
    shift_amount->type = OPERAND_LIT;
    shift_amount->lit_value = 16;
    shift_amount->asm_type = &kWordType;

    struct AsmInstr* shift = arena_alloc(sizeof(struct AsmInstr));
    shift->type = ASM_BINARY;
    shift->alu_op = ALU_LSR;
    shift->dst = tmp_reg;
    shift->src1 = tmp_reg;
    shift->src2 = shift_amount;
    shift->next = NULL;
    append_asm_instr(&head, &tail, shift);

    struct AsmInstr* store_high = arena_alloc(sizeof(struct AsmInstr));
    store_high->type = ASM_MOV;
    store_high->dst = add_offset_typed(dst, 2, &kByteType);
    store_high->src1 = tmp_reg;
    store_high->src2 = NULL;
    store_high->next = NULL;
    append_asm_instr(&head, &tail, store_high);

    return head;
  } else {
    asm_gen_error1_size("copy-bytes", func_name,
                        "unsupported register copy size %zu; expected 1, 2, or 4",
                        size);
  }

  struct AsmInstr* mov = arena_alloc(sizeof(struct AsmInstr));
  mov->type = ASM_MOV;
  mov->dst = add_offset_typed(dst, 0, chunk_type);
  mov->src1 = arena_alloc(sizeof(struct Operand));
  mov->src1->type = OPERAND_REG;
  mov->src1->reg = src_reg;
  mov->src1->asm_type = chunk_type;
  mov->src2 = NULL;
  mov->next = NULL;
  return mov;
}

struct AsmSymbolTable* convert_symbol_table(struct SymbolTable* symbols){
  struct AsmSymbolTable* asm_table = create_asm_symbol_table(symbols->size);
  
  for (size_t i = 0; i < symbols->size; i++){
    struct SymbolEntry* cur = symbols->arr[i];
    while (cur != NULL){
      struct AsmType* asm_type = type_to_asm_type(cur->type);
      bool is_static = cur->attrs != NULL &&
                       (cur->attrs->attr_type == STATIC_ATTR ||
                        cur->attrs->attr_type == CONST_ATTR);
      bool is_defined = cur->attrs != NULL && cur->attrs->is_defined;
      bool return_on_stack = false;
      if (cur->type != NULL && cur->type->type == FUN_TYPE) {
        struct Type* ret_type = cur->type->type_data.fun_type.return_type;
        if (ret_type != NULL &&
            (ret_type->type == STRUCT_TYPE || ret_type->type == UNION_TYPE)) {
          struct TypeEntry* entry = type_table_get(global_type_table,
            ret_type->type == STRUCT_TYPE ? ret_type->type_data.struct_type.name
                                          : ret_type->type_data.union_type.name);
          if (entry == NULL) {
            asm_gen_error0("symbol table", cur->key,
                           "missing type entry for aggregate return");
          }
          struct StructEntry* agg_entry =
              (entry->type == STRUCT_ENTRY) ? entry->data.struct_entry
                                            : entry->data.union_entry;
          struct VarClassList* classes = classify_struct(agg_entry);
          return_on_stack = (classes->var_class == MEMORY_CLASS);
        }
      }
      
      asm_symbol_table_insert(asm_table, cur->key, asm_type, is_static, is_defined, return_on_stack);
      cur = cur->next;
    }
  }
  
  return asm_table;
}

// Purpose: Check whether a slice contains the compiler temp marker.
// Inputs: name is a slice of the symbol name.
// Outputs: Returns true if the marker appears in the slice.
// Invariants/Assumptions: name->start may not be NUL-terminated.
static bool slice_contains_temp_marker(struct Slice* name) {
  if (name == NULL || name->start == NULL || name->len < kTempMarkerLen) {
    return false;
  }
  for (size_t i = 0; i + kTempMarkerLen <= name->len; i++) {
    bool match = true;
    for (size_t j = 0; j < kTempMarkerLen; j++) {
      if (name->start[i + j] != kTempMarker[j]) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
}

// Purpose: Append a debug local entry into a list sorted by stack offset.
// Inputs: head is the list head; entry is the node to insert.
// Outputs: Updates the head pointer to keep ascending offset order.
// Invariants/Assumptions: entry is not already in the list.
static void insert_debug_local_sorted(struct DebugLocal** head, struct DebugLocal* entry) {
  if (head == NULL || entry == NULL) {
    return;
  }
  if (*head == NULL || entry->offset < (*head)->offset) {
    entry->next = *head;
    *head = entry;
    return;
  }
  struct DebugLocal* cur = *head;
  while (cur->next != NULL && cur->next->offset <= entry->offset) {
    cur = cur->next;
  }
  entry->next = cur->next;
  cur->next = entry;
}

// Purpose: Collect stack-local debug metadata from a pseudo map.
// Inputs: map is the pseudo map for a single function.
// Outputs: Returns a sorted list of locals and sets out_count.
// Invariants/Assumptions: map entries map pseudos to stack or static storage.
static struct DebugLocal* collect_debug_locals(struct PseudoMap* map, size_t* out_count) {
  if (out_count != NULL) {
    *out_count = 0;
  }
  if (map == NULL || map->arr == NULL) {
    return NULL;
  }
  struct DebugLocal* head = NULL;
  for (size_t i = 0; i < map->size; i++) {
    for (struct PseudoEntry* entry = map->arr[i]; entry != NULL; entry = entry->next) {
      if (entry->pseudo == NULL || entry->mapped == NULL) {
        continue;
      }
      struct Slice* name = entry->pseudo->pseudo;
      if (slice_contains_temp_marker(name)) {
        continue;
      }
      if (entry->mapped->type != OPERAND_MEMORY || entry->mapped->reg != BP) {
        continue;
      }
      struct DebugLocal* local = arena_alloc(sizeof(struct DebugLocal));
      local->name = name;
      local->offset = entry->mapped->lit_value;
      local->size = asm_type_size(entry->mapped->asm_type); // store size in bytes
      local->next = NULL;
      insert_debug_local_sorted(&head, local);
      if (out_count != NULL) {
        (*out_count)++;
      }
    }
  }
  return head;
}

// Purpose: Detect whether a function body contains debug markers.
// Inputs: instrs is the function instruction list.
// Outputs: Returns true if at least one debug boundary is present.
// Invariants/Assumptions: instrs may be NULL for empty bodies.
static bool asm_has_debug_markers(struct AsmInstr* instrs) {
  for (struct AsmInstr* cur = instrs; cur != NULL; cur = cur->next) {
    if (cur->type == ASM_BOUNDARY) {
      return true;
    }
  }
  return false;
}

// Purpose: Print a slice to the selected file descriptor.
// Inputs: slice may be NULL; otherwise points to a valid slice.
// Outputs: Writes a best-effort identifier representation to fd.
// Invariants/Assumptions: slice->start may be non-null-terminated.
static void asm_gen_fdprint_slice(int fd, struct Slice* slice) {
  int args[2];

  if (slice == NULL || slice->start == NULL) {
    fdputs(fd, "<null>");
    return;
  }
  args[0] = (int)slice->len;
  args[1] = (int)slice->start;
  fdprintf(fd, "%.*s", args);
}

// Purpose: Print a slice to stderr for error reporting.
// Inputs: slice may be NULL; otherwise points to a valid slice.
// Outputs: Writes a best-effort identifier representation to stderr.
// Invariants/Assumptions: slice->start may be non-null-terminated.
static void asm_gen_fprint_slice(struct Slice* slice) {
  asm_gen_fdprint_slice(STDERR, slice);
}

// Purpose: Emit the shared prefix for ASM generation diagnostics.
// Inputs: operation labels the failing step; func_name may be NULL.
// Outputs: Writes the shared diagnostic prefix to stderr.
// Invariants/Assumptions: Callers append the specific message and newline.
static void asm_gen_error_prefix(char* operation, struct Slice* func_name) {
  int args[1];

  fdputs(STDERR, "ASM generation error");
  if (operation != NULL) {
    args[0] = (int)operation;
    fdprintf(STDERR, " (%s)", args);
  }
  if (func_name != NULL) {
    fdputs(STDERR, " in ");
    asm_gen_fprint_slice(func_name);
  }
  fdputs(STDERR, ": ");
}

static void asm_gen_error0(char* operation, struct Slice* func_name, char* message) {
  asm_gen_error_prefix(operation, func_name);
  fdputs(STDERR, message);
  fdputs(STDERR, "\n");
  exit(1);
}

static void asm_gen_error1_int(char* operation, struct Slice* func_name, char* fmt, int arg0) {
  int args[1];

  args[0] = arg0;
  asm_gen_error_prefix(operation, func_name);
  fdprintf(STDERR, fmt, args);
  fdputs(STDERR, "\n");
  exit(1);
}

static void asm_gen_error1_size(char* operation, struct Slice* func_name, char* fmt, size_t arg0) {
  int args[1];

  args[0] = (int)arg0;
  asm_gen_error_prefix(operation, func_name);
  fdprintf(STDERR, fmt, args);
  fdputs(STDERR, "\n");
  exit(1);
}

static void asm_gen_error1_slice(char* operation, struct Slice* func_name, char* fmt,
                                 struct Slice* slice) {
  int args[2];

  asm_gen_error_prefix(operation, func_name);
  if (slice == NULL || slice->start == NULL) {
    args[0] = 6;
    args[1] = (int)"<null>";
  } else {
    args[0] = (int)slice->len;
    args[1] = (int)slice->start;
  }
  fdprintf(STDERR, fmt, args);
  fdputs(STDERR, "\n");
  exit(1);
}

static void asm_gen_error1_len_str(char* operation, struct Slice* func_name, char* fmt,
                                   int len, char* str) {
  int args[2];

  args[0] = len;
  args[1] = (int)str;
  asm_gen_error_prefix(operation, func_name);
  fdprintf(STDERR, fmt, args);
  fdputs(STDERR, "\n");
  exit(1);
}

static void asm_gen_error2_size(char* operation, struct Slice* func_name, char* fmt,
                                size_t arg0, size_t arg1) {
  int args[2];

  args[0] = (int)arg0;
  args[1] = (int)arg1;
  asm_gen_error_prefix(operation, func_name);
  fdprintf(STDERR, fmt, args);
  fdputs(STDERR, "\n");
  exit(1);
}

static void asm_gen_error2_int_str(char* operation, struct Slice* func_name, char* fmt,
                                   int arg0, char* arg1) {
  int args[2];

  args[0] = arg0;
  args[1] = (int)arg1;
  asm_gen_error_prefix(operation, func_name);
  fdprintf(STDERR, fmt, args);
  fdputs(STDERR, "\n");
  exit(1);
}

// Purpose: Name a TAC instruction type for diagnostics.
// Inputs: type is the TAC instruction enum value.
// Outputs: Returns a string literal describing the TAC opcode.
// Invariants/Assumptions: Unknown values map to "TAC<unknown>".
static char* tac_instr_name(enum TACInstrType type) {
  switch (type) {
    case TACRETURN:
      return "TACRETURN";
    case TACUNARY:
      return "TACUNARY";
    case TACBINARY:
      return "TACBINARY";
    case TACCOND_JUMP:
      return "TACCOND_JUMP";
    case TACCMP:
      return "TACCMP";
    case TACJUMP:
      return "TACJUMP";
    case TACLABEL:
      return "TACLABEL";
    case TACCOPY:
      return "TACCOPY";
    case TACCALL:
      return "TACCALL";
    case TACCALL_INDIRECT:
      return "TACCALL_INDIRECT";
    case TACGET_ADDRESS:
      return "TACGET_ADDRESS";
    case TACLOAD:
      return "TACLOAD";
    case TACSTORE:
      return "TACSTORE";
    case TACCOPY_TO_OFFSET:
      return "TACCOPY_TO_OFFSET";
    default:
      return "TAC<unknown>";
  }
}

// Purpose: Append a top-level ASM node to the program list.
// Inputs: prog is the ASM program; node is the top-level to append.
// Outputs: Updates prog->head/tail to include node.
// Invariants/Assumptions: node->next is either NULL or a valid list tail.
static void append_asm_top_level(struct AsmProg* prog, struct AsmTopLevel* node) {
  if (prog == NULL || node == NULL) {
    asm_gen_error0("top-level", NULL, "append requested with NULL program or node");
  }

  if (prog->head == NULL) {
    prog->head = node;
    prog->tail = node;
    return;
  }

  prog->tail->next = node;
  prog->tail = node;
}

// Purpose: Lower a TAC program into the ASM IR representation.
// Inputs: tac_prog is the TAC program to lower (must be non-NULL);
//         emit_sections controls whether .data/.text directives are emitted.
// Outputs: Returns a newly allocated ASM program rooted in arena storage.
// Invariants/Assumptions: TAC top-level lists are well-formed and acyclic.
struct AsmProg* prog_to_asm(struct TACProg* tac_prog, bool emit_sections) {
  if (tac_prog == NULL) {
    asm_gen_error0("program", NULL, "input TAC program is NULL");
  }

  asm_symbol_table = convert_symbol_table(global_symbol_table);

  struct AsmProg* asm_prog = arena_alloc(sizeof(struct AsmProg));
  asm_prog->head = NULL;
  asm_prog->tail = NULL;

  if (emit_sections) {
    // emit .data
    struct AsmTopLevel* data_directive = arena_alloc(sizeof(struct AsmTopLevel));
    data_directive->type = ASM_SECTION;
    data_directive->name = &data_directive_slice;
    data_directive->locals = NULL;
    data_directive->num_locals = 0;
    data_directive->next = NULL;
    append_asm_top_level(asm_prog, data_directive);
  } else {
    struct AsmTopLevel* align_directive = arena_alloc(sizeof(struct AsmTopLevel));
    align_directive->type = ASM_ALIGN;
    align_directive->alignment = 4; // word-align functions
    align_directive->next = NULL;
    append_asm_top_level(asm_prog, align_directive);
  }

  for (struct TopLevel* tac_top = tac_prog->statics; tac_top != NULL; tac_top = tac_top->next) {
    struct AsmTopLevel* asm_top = top_level_to_asm(tac_top);
    if (asm_top == NULL) {
      asm_gen_error0("top-level", NULL, "failed to lower TAC static");
    }
    append_asm_top_level(asm_prog, asm_top);
  }

  if (emit_sections) {
    // emit .text
    struct AsmTopLevel* text_directive = arena_alloc(sizeof(struct AsmTopLevel));
    text_directive->type = ASM_SECTION;
    text_directive->name = &text_directive_slice;
    text_directive->locals = NULL;
    text_directive->num_locals = 0;
    text_directive->next = NULL;
    append_asm_top_level(asm_prog, text_directive);
  } else {
    struct AsmTopLevel* align_directive = arena_alloc(sizeof(struct AsmTopLevel));
    align_directive->type = ASM_ALIGN;
    align_directive->alignment = 4; // word-align functions
    align_directive->next = NULL;
    append_asm_top_level(asm_prog, align_directive);
  }

  for (struct TopLevel* tac_top = tac_prog->head; tac_top != NULL; tac_top = tac_top->next) {
      
    struct AsmTopLevel* asm_top = top_level_to_asm(tac_top);
    if (asm_top == NULL) {
      asm_gen_error0("top-level", NULL, "failed to lower TAC top-level");
    }
    append_asm_top_level(asm_prog, asm_top);
  }

  if (pseudo_map != NULL) {
    destroy_pseudo_map(pseudo_map);
    pseudo_map = NULL;
  }

  return asm_prog;
}

struct AsmTopLevel* top_level_to_asm(struct TopLevel* tac_top) {
  if (tac_top == NULL) {
    asm_gen_error0("top-level", NULL, "NULL TAC top-level encountered");
  }

  struct AsmTopLevel* asm_top = arena_alloc(sizeof(struct AsmTopLevel));
  asm_top->next = NULL;
  asm_top->locals = NULL;
  asm_top->num_locals = 0;
  
  if (tac_top->type == FUNC) {
    asm_top->type = ASM_FUNC;
    asm_top->name = tac_top->name;
    asm_top->global = tac_top->global;

    struct AsmSymbolEntry* func_entry = asm_symbol_table_get(asm_symbol_table, tac_top->name);
    if (func_entry == NULL) {
      asm_gen_error0("top-level", tac_top->name,
                     "function symbol not found in ASM symbol table");
    }
    bool return_in_memory = false;
    struct SymbolEntry* sym_entry = symbol_table_get(global_symbol_table, tac_top->name);
    if (sym_entry == NULL || sym_entry->type == NULL || sym_entry->type->type != FUN_TYPE) {
      asm_gen_error1_slice("top-level", tac_top->name,
                           "missing function type information for %.*s",
                           tac_top->name);
    }
    struct Type* ret_type = sym_entry->type->type_data.fun_type.return_type;
    if (ret_type != NULL && ret_type->type != VOID_TYPE) {
      struct Val ret_val;
      ret_val.val_type = VARIABLE;
      ret_val.val.var_name = tac_top->name;
      ret_val.type = ret_type;
      struct OperandList* ignored = NULL;
      classify_return_val(&ret_val, &ignored, &return_in_memory);
    }

    struct AsmInstr* asm_body = set_up_params(asm_top->name, tac_top->params, tac_top->num_params, return_in_memory);

    // prepend stack allocation instruction
    // ASM:
    // Binary Sub SP, SP, <stack_bytes>
    struct AsmInstr* alloc_instr = arena_alloc(sizeof(struct AsmInstr));
    alloc_instr->type = ASM_BINARY;
    alloc_instr->alu_op = ALU_SUB;
    alloc_instr->dst = arena_alloc(sizeof(struct Operand));
    alloc_instr->dst->type = OPERAND_REG;
    alloc_instr->dst->reg = SP;
    alloc_instr->dst->lit_value = 0;
    alloc_instr->dst->asm_type = &kWordType;
    alloc_instr->src1 = arena_alloc(sizeof(struct Operand));
    alloc_instr->src1->type = OPERAND_REG;
    alloc_instr->src1->reg = SP;
    alloc_instr->src1->asm_type = &kWordType;
    alloc_instr->src2 = arena_alloc(sizeof(struct Operand));
    alloc_instr->src2->type = OPERAND_LIT;
    alloc_instr->src2->lit_value = 0; // placeholder, to be filled after stack size calculation
    alloc_instr->src2->asm_type = &kWordType;
    alloc_instr->next = asm_body;
    asm_body = alloc_instr;

    // convert body instructions
    for (struct TACInstr* tac_instr = tac_top->body; tac_instr != NULL; tac_instr = tac_instr->next) {
      struct AsmInstr* asm_instr = instr_to_asm(tac_top->name, tac_instr);
      if (asm_body == NULL) {
        asm_body = asm_instr;
      } else {
        // append to asm_body
        struct AsmInstr* last = asm_body;
        while (last->next != NULL) {
          last = last->next;
        }
        last->next = asm_instr;
      }
    }

    size_t reserved_bytes = return_in_memory ? kStackSlotBytes : 0;
    size_t stack_size = create_maps(asm_body, reserved_bytes);
    //print_pseudo_map(asm_top->name, pseudo_map);
    
    if (asm_has_debug_markers(asm_body)) {
      asm_top->locals = collect_debug_locals(pseudo_map, &asm_top->num_locals);
    }
    asm_body->src2->lit_value = (int)stack_size; // update stack allocation size

    replace_pseudo(asm_body);

    // Pseudo maps are per-function; reclaim them after lowering.
    if (pseudo_map != NULL) {
      destroy_pseudo_map(pseudo_map);
      pseudo_map = NULL;
    }

    asm_top->body = asm_body;

    return asm_top;
  } else if (tac_top->type == STATIC_VAR) {
    asm_top->type = ASM_STATIC_VAR;
    asm_top->name = tac_top->name;
    asm_top->global = tac_top->global;

    asm_top->alignment = type_alignment(tac_top->var_type, tac_top->name);
    asm_top->init_values = tac_top->init_values;

    return asm_top;
  } else if (tac_top->type == STATIC_CONST) {
    asm_top->type = ASM_STATIC_CONST;
    asm_top->name = tac_top->name;
    asm_top->global = tac_top->global;

    asm_top->alignment = type_alignment(tac_top->var_type, tac_top->name);
    asm_top->init_values = tac_top->init_values;

    return asm_top;
  } else {
    asm_gen_error1_int("top-level", tac_top->name,
                       "unknown top-level type %d", (int)tac_top->type);
    return NULL;
  }
}

// Purpose: Lower a direct TAC call into an ASM instruction list.
// Inputs: func_name identifies the owning function; call_instr describes the call.
// Outputs: Returns the full ASM sequence for argument setup, call, and return moves.
// Invariants/Assumptions: The call has already been typechecked against the ABI classifier.
static struct AsmInstr* direct_call_to_asm(struct Slice* func_name,
                                           struct TACCall* call_instr) {
  struct AsmInstr* head = NULL;
  struct AsmInstr* tail = NULL;

  bool return_in_memory = false;
  struct OperandList* dests = NULL;
  size_t reg_index = 0;

  if (call_instr->dst != NULL) {
    classify_return_val(call_instr->dst, &dests, &return_in_memory);
  }

  if (return_in_memory) {
    struct Operand* dst_operand = tac_val_to_asm(call_instr->dst);
    struct AsmInstr* addr_instr = arena_alloc(sizeof(struct AsmInstr));
    addr_instr->type = ASM_GET_ADDRESS;
    addr_instr->dst = arena_alloc(sizeof(struct Operand));
    addr_instr->dst->type = OPERAND_REG;
    addr_instr->dst->reg = R1;
    addr_instr->src1 = dst_operand;
    addr_instr->src2 = NULL;
    addr_instr->next = NULL;
    append_asm_instr(&head, &tail, addr_instr);

    reg_index = 1; // R1 is reserved for the hidden return pointer.
  }

  struct OperandList* reg_args = NULL;
  struct OperandList* stack_args = NULL;
  classify_params(call_instr->args, call_instr->num_args, return_in_memory,
      &reg_args, &stack_args);

  for (struct OperandList* reg_arg_iter = reg_args;
       reg_arg_iter != NULL;
       reg_arg_iter = reg_arg_iter->next) {
    struct Operand* arg = reg_arg_iter->opr;
    size_t arg_size = asm_type_size(arg->asm_type);
    size_t arg_alignment = operand_base_alignment(arg);
    bool needs_byte_copy = arg->asm_type->type == BYTE_ARRAY || arg_alignment < arg_size;

    if (needs_byte_copy) {
      struct AsmInstr* copy_instrs =
          copy_bytes_to_reg(func_name, arg, (enum Reg)(R1 + reg_index), arg_size);
      append_asm_instrs(&head, &tail, copy_instrs);
    } else {
      struct AsmInstr* mov_instr = arena_alloc(sizeof(struct AsmInstr));
      mov_instr->type = ASM_MOV;
      mov_instr->dst = arena_alloc(sizeof(struct Operand));
      mov_instr->dst->type = OPERAND_REG;
      mov_instr->dst->reg = (enum Reg)(R1 + reg_index);
      mov_instr->dst->asm_type = arg->asm_type;
      mov_instr->src1 = arg;
      mov_instr->src2 = NULL;
      mov_instr->next = NULL;
      append_asm_instr(&head, &tail, mov_instr);
    }

    reg_index++;
  }

  size_t stack_bytes = 0;
  size_t stack_arg_count = operand_list_length(stack_args);
  for (size_t idx = stack_arg_count; idx > 0;) {
    idx--;
    struct Operand* arg = operand_list_get(stack_args, idx);
    size_t copy_size = asm_type_size(arg->asm_type);
    size_t arg_alignment = operand_base_alignment(arg);
    bool needs_byte_copy = arg->asm_type->type == BYTE_ARRAY ||
                           copy_size < kStackSlotBytes ||
                           arg_alignment < copy_size;

    if (needs_byte_copy) {
      if (copy_size > kStackSlotBytes) {
        asm_gen_error2_size("call", func_name,
                            "stack arg chunk exceeds %zu-byte slot (size=%zu)",
                            kStackSlotBytes, copy_size);
      }
      stack_bytes += kStackSlotBytes;

      struct AsmInstr* alloc_instr = arena_alloc(sizeof(struct AsmInstr));
      alloc_instr->type = ASM_BINARY;
      alloc_instr->alu_op = ALU_SUB;
      alloc_instr->dst = arena_alloc(sizeof(struct Operand));
      alloc_instr->dst->type = OPERAND_REG;
      alloc_instr->dst->reg = SP;
      alloc_instr->dst->asm_type = &kWordType;
      alloc_instr->src1 = arena_alloc(sizeof(struct Operand));
      alloc_instr->src1->type = OPERAND_REG;
      alloc_instr->src1->reg = SP;
      alloc_instr->src1->asm_type = &kWordType;
      alloc_instr->src2 = arena_alloc(sizeof(struct Operand));
      alloc_instr->src2->type = OPERAND_LIT;
      alloc_instr->src2->lit_value = (int)kStackSlotBytes;
      alloc_instr->src2->asm_type = &kWordType;
      alloc_instr->next = NULL;
      append_asm_instr(&head, &tail, alloc_instr);

      append_asm_instrs(&head, &tail,
                        copy_bytes(func_name, arg, stack_mem_operand(), copy_size));
    } else {
      struct AsmInstr* push_instr = arena_alloc(sizeof(struct AsmInstr));
      push_instr->type = ASM_PUSH;
      push_instr->src1 = arg;
      push_instr->src2 = NULL;
      push_instr->dst = NULL;
      push_instr->next = NULL;
      stack_bytes += kStackSlotBytes;
      append_asm_instr(&head, &tail, push_instr);
    }
  }

  struct AsmInstr* call_asm = arena_alloc(sizeof(struct AsmInstr));
  call_asm->type = ASM_CALL;
  call_asm->label = call_instr->func_name;
  call_asm->dst = NULL;
  call_asm->src1 = NULL;
  call_asm->src2 = NULL;
  call_asm->next = NULL;
  append_asm_instr(&head, &tail, call_asm);

  if (stack_bytes > 0) {
    struct AsmInstr* stack_adjust = arena_alloc(sizeof(struct AsmInstr));
    stack_adjust->type = ASM_BINARY;
    stack_adjust->alu_op = ALU_ADD;
    stack_adjust->dst = arena_alloc(sizeof(struct Operand));
    stack_adjust->dst->type = OPERAND_REG;
    stack_adjust->dst->reg = SP;
    stack_adjust->dst->asm_type = &kWordType;
    stack_adjust->src1 = arena_alloc(sizeof(struct Operand));
    stack_adjust->src1->type = OPERAND_REG;
    stack_adjust->src1->reg = SP;
    stack_adjust->src1->asm_type = &kWordType;
    stack_adjust->src2 = arena_alloc(sizeof(struct Operand));
    stack_adjust->src2->type = OPERAND_LIT;
    stack_adjust->src2->lit_value = stack_bytes;
    stack_adjust->src2->asm_type = &kWordType;
    stack_adjust->next = NULL;
    append_asm_instr(&head, &tail, stack_adjust);
  }

  if (call_instr->dst != NULL && !return_in_memory) {
    reg_index = 0;
    for (struct OperandList* dest_iter = dests; dest_iter != NULL; dest_iter = dest_iter->next) {
      struct Operand* dest = dest_iter->opr;
      enum Reg ret_reg = (enum Reg)(R1 + reg_index);
      size_t dest_size = asm_type_size(dest->asm_type);
      size_t dest_alignment = operand_base_alignment(dest);
      bool needs_byte_copy = dest->asm_type->type == BYTE_ARRAY ||
                             dest_alignment < dest_size;

      if (needs_byte_copy) {
        struct AsmInstr* copy_instrs =
            copy_bytes_from_reg(func_name, ret_reg, dest, dest_size);
        append_asm_instrs(&head, &tail, copy_instrs);
      } else {
        struct AsmInstr* mov_instr = arena_alloc(sizeof(struct AsmInstr));
        mov_instr->type = ASM_MOV;
        mov_instr->dst = dest;
        mov_instr->src1 = arena_alloc(sizeof(struct Operand));
        mov_instr->src1->type = OPERAND_REG;
        mov_instr->src1->reg = ret_reg;
        mov_instr->src1->asm_type = dest->asm_type;
        mov_instr->src2 = NULL;
        mov_instr->next = NULL;
        append_asm_instr(&head, &tail, mov_instr);
      }

      reg_index++;
    }
  }

  return head;
}

// Purpose: Lower an indirect TAC call into an ASM instruction list.
// Inputs: func_name identifies the owning function; call_instr describes the call.
// Outputs: Returns the full ASM sequence for argument setup, call, and return moves.
// Invariants/Assumptions: The callee operand already holds a callable address.
static struct AsmInstr* indirect_call_to_asm(struct Slice* func_name,
                                             struct TACCallIndirect* call_instr) {
  struct AsmInstr* head = NULL;
  struct AsmInstr* tail = NULL;

  bool return_in_memory = false;
  struct OperandList* dests = NULL;
  size_t reg_index = 0;

  if (call_instr->dst != NULL) {
    classify_return_val(call_instr->dst, &dests, &return_in_memory);
  }

  if (return_in_memory) {
    struct Operand* dst_operand = tac_val_to_asm(call_instr->dst);
    struct AsmInstr* addr_instr = arena_alloc(sizeof(struct AsmInstr));
    addr_instr->type = ASM_GET_ADDRESS;
    addr_instr->dst = arena_alloc(sizeof(struct Operand));
    addr_instr->dst->type = OPERAND_REG;
    addr_instr->dst->reg = R1;
    addr_instr->src1 = dst_operand;
    addr_instr->src2 = NULL;
    addr_instr->next = NULL;
    append_asm_instr(&head, &tail, addr_instr);

    reg_index = 1; // R1 is reserved for the hidden return pointer.
  }

  struct OperandList* reg_args = NULL;
  struct OperandList* stack_args = NULL;
  classify_params(call_instr->args, call_instr->num_args, return_in_memory,
      &reg_args, &stack_args);

  for (struct OperandList* reg_arg_iter = reg_args;
       reg_arg_iter != NULL;
       reg_arg_iter = reg_arg_iter->next) {
    struct Operand* arg = reg_arg_iter->opr;
    size_t arg_size = asm_type_size(arg->asm_type);
    size_t arg_alignment = operand_base_alignment(arg);
    bool needs_byte_copy = arg->asm_type->type == BYTE_ARRAY || arg_alignment < arg_size;

    if (needs_byte_copy) {
      struct AsmInstr* copy_instrs =
          copy_bytes_to_reg(func_name, arg, (enum Reg)(R1 + reg_index), arg_size);
      append_asm_instrs(&head, &tail, copy_instrs);
    } else {
      struct AsmInstr* mov_instr = arena_alloc(sizeof(struct AsmInstr));
      mov_instr->type = ASM_MOV;
      mov_instr->dst = arena_alloc(sizeof(struct Operand));
      mov_instr->dst->type = OPERAND_REG;
      mov_instr->dst->reg = (enum Reg)(R1 + reg_index);
      mov_instr->dst->asm_type = arg->asm_type;
      mov_instr->src1 = arg;
      mov_instr->src2 = NULL;
      mov_instr->next = NULL;
      append_asm_instr(&head, &tail, mov_instr);
    }

    reg_index++;
  }

  size_t stack_bytes = 0;
  size_t stack_arg_count = operand_list_length(stack_args);
  for (size_t idx = stack_arg_count; idx > 0;) {
    idx--;
    struct Operand* arg = operand_list_get(stack_args, idx);
    size_t copy_size = asm_type_size(arg->asm_type);
    size_t arg_alignment = operand_base_alignment(arg);
    bool needs_byte_copy = arg->asm_type->type == BYTE_ARRAY ||
                           copy_size < kStackSlotBytes ||
                           arg_alignment < copy_size;

    if (needs_byte_copy) {
      if (copy_size > kStackSlotBytes) {
        asm_gen_error2_size("call", func_name,
                            "stack arg chunk exceeds %zu-byte slot (size=%zu)",
                            kStackSlotBytes, copy_size);
      }
      stack_bytes += kStackSlotBytes;

      struct AsmInstr* alloc_instr = arena_alloc(sizeof(struct AsmInstr));
      alloc_instr->type = ASM_BINARY;
      alloc_instr->alu_op = ALU_SUB;
      alloc_instr->dst = arena_alloc(sizeof(struct Operand));
      alloc_instr->dst->type = OPERAND_REG;
      alloc_instr->dst->reg = SP;
      alloc_instr->dst->asm_type = &kWordType;
      alloc_instr->src1 = arena_alloc(sizeof(struct Operand));
      alloc_instr->src1->type = OPERAND_REG;
      alloc_instr->src1->reg = SP;
      alloc_instr->src1->asm_type = &kWordType;
      alloc_instr->src2 = arena_alloc(sizeof(struct Operand));
      alloc_instr->src2->type = OPERAND_LIT;
      alloc_instr->src2->lit_value = (int)kStackSlotBytes;
      alloc_instr->src2->asm_type = &kWordType;
      alloc_instr->next = NULL;
      append_asm_instr(&head, &tail, alloc_instr);

      append_asm_instrs(&head, &tail,
                        copy_bytes(func_name, arg, stack_mem_operand(), copy_size));
    } else {
      struct AsmInstr* push_instr = arena_alloc(sizeof(struct AsmInstr));
      push_instr->type = ASM_PUSH;
      push_instr->src1 = arg;
      push_instr->src2 = NULL;
      push_instr->dst = NULL;
      push_instr->next = NULL;
      stack_bytes += kStackSlotBytes;
      append_asm_instr(&head, &tail, push_instr);
    }
  }

  struct AsmInstr* call_asm = arena_alloc(sizeof(struct AsmInstr));
  call_asm->type = ASM_INDIRECT_CALL;
  call_asm->dst = NULL;
  call_asm->src1 = tac_val_to_asm(call_instr->func);
  call_asm->src2 = NULL;
  call_asm->next = NULL;
  append_asm_instr(&head, &tail, call_asm);

  if (stack_bytes > 0) {
    struct AsmInstr* stack_adjust = arena_alloc(sizeof(struct AsmInstr));
    stack_adjust->type = ASM_BINARY;
    stack_adjust->alu_op = ALU_ADD;
    stack_adjust->dst = arena_alloc(sizeof(struct Operand));
    stack_adjust->dst->type = OPERAND_REG;
    stack_adjust->dst->reg = SP;
    stack_adjust->dst->asm_type = &kWordType;
    stack_adjust->src1 = arena_alloc(sizeof(struct Operand));
    stack_adjust->src1->type = OPERAND_REG;
    stack_adjust->src1->reg = SP;
    stack_adjust->src1->asm_type = &kWordType;
    stack_adjust->src2 = arena_alloc(sizeof(struct Operand));
    stack_adjust->src2->type = OPERAND_LIT;
    stack_adjust->src2->lit_value = stack_bytes;
    stack_adjust->src2->asm_type = &kWordType;
    stack_adjust->next = NULL;
    append_asm_instr(&head, &tail, stack_adjust);
  }

  if (call_instr->dst != NULL && !return_in_memory) {
    reg_index = 0;
    for (struct OperandList* dest_iter = dests; dest_iter != NULL; dest_iter = dest_iter->next) {
      struct Operand* dest = dest_iter->opr;
      enum Reg ret_reg = (enum Reg)(R1 + reg_index);
      size_t dest_size = asm_type_size(dest->asm_type);
      size_t dest_alignment = operand_base_alignment(dest);
      bool needs_byte_copy = dest->asm_type->type == BYTE_ARRAY ||
                             dest_alignment < dest_size;

      if (needs_byte_copy) {
        struct AsmInstr* copy_instrs =
            copy_bytes_from_reg(func_name, ret_reg, dest, dest_size);
        append_asm_instrs(&head, &tail, copy_instrs);
      } else {
        struct AsmInstr* mov_instr = arena_alloc(sizeof(struct AsmInstr));
        mov_instr->type = ASM_MOV;
        mov_instr->dst = dest;
        mov_instr->src1 = arena_alloc(sizeof(struct Operand));
        mov_instr->src1->type = OPERAND_REG;
        mov_instr->src1->reg = ret_reg;
        mov_instr->src1->asm_type = dest->asm_type;
        mov_instr->src2 = NULL;
        mov_instr->next = NULL;
        append_asm_instr(&head, &tail, mov_instr);
      }

      reg_index++;
    }
  }

  return head;
}

// Purpose: Lower a static-symbol load through an explicit address calculation.
// Inputs: func_name identifies the owning function; base/base_type name the static object.
// Outputs: Returns GET_ADDRESS + ADD + LOAD using a temp pointer.
// Invariants/Assumptions: offset is non-zero and base_type describes the underlying symbol.
static struct AsmInstr* load_from_static_offset_to_asm(struct Slice* func_name,
                                                       struct Slice* base,
                                                       struct AsmType* base_type,
                                                       int offset,
                                                       struct Operand* dst) {
  struct AsmInstr* head = NULL;
  struct AsmInstr* tail = NULL;
  struct Operand* addr_temp = make_asm_temp(func_name, &kWordType);
  struct Operand* base_opr = make_pseudo_mem(base, base_type, 0);

  struct AsmInstr* get_addr = arena_alloc(sizeof(struct AsmInstr));
  get_addr->type = ASM_GET_ADDRESS;
  get_addr->dst = addr_temp;
  get_addr->src1 = base_opr;
  get_addr->src2 = NULL;
  get_addr->next = NULL;
  append_asm_instr(&head, &tail, get_addr);

  struct AsmInstr* add_off = arena_alloc(sizeof(struct AsmInstr));
  add_off->type = ASM_BINARY;
  add_off->alu_op = ALU_ADD;
  add_off->dst = addr_temp;
  add_off->src1 = addr_temp;
  add_off->src2 = arena_alloc(sizeof(struct Operand));
  add_off->src2->type = OPERAND_LIT;
  add_off->src2->lit_value = offset;
  add_off->src2->asm_type = &kWordType;
  add_off->next = NULL;
  append_asm_instr(&head, &tail, add_off);

  struct AsmInstr* load_instr = arena_alloc(sizeof(struct AsmInstr));
  load_instr->type = ASM_LOAD;
  load_instr->dst = dst;
  load_instr->src1 = addr_temp;
  load_instr->src2 = NULL;
  load_instr->next = NULL;
  append_asm_instr(&head, &tail, load_instr);

  return head;
}

// Purpose: Lower a static-symbol store through an explicit address calculation.
// Inputs: func_name identifies the owning function; base/base_type name the static object.
// Outputs: Returns GET_ADDRESS + ADD + STORE using a temp pointer.
// Invariants/Assumptions: offset is non-zero and base_type describes the underlying symbol.
static struct AsmInstr* store_to_static_offset_to_asm(struct Slice* func_name,
                                                      struct Slice* base,
                                                      struct AsmType* base_type,
                                                      int offset,
                                                      struct Operand* src) {
  struct AsmInstr* head = NULL;
  struct AsmInstr* tail = NULL;
  struct Operand* addr_temp = make_asm_temp(func_name, &kWordType);
  struct Operand* base_opr = make_pseudo_mem(base, base_type, 0);

  struct AsmInstr* get_addr = arena_alloc(sizeof(struct AsmInstr));
  get_addr->type = ASM_GET_ADDRESS;
  get_addr->dst = addr_temp;
  get_addr->src1 = base_opr;
  get_addr->src2 = NULL;
  get_addr->next = NULL;
  append_asm_instr(&head, &tail, get_addr);

  struct AsmInstr* add_off = arena_alloc(sizeof(struct AsmInstr));
  add_off->type = ASM_BINARY;
  add_off->alu_op = ALU_ADD;
  add_off->dst = addr_temp;
  add_off->src1 = addr_temp;
  add_off->src2 = arena_alloc(sizeof(struct Operand));
  add_off->src2->type = OPERAND_LIT;
  add_off->src2->lit_value = offset;
  add_off->src2->asm_type = &kWordType;
  add_off->next = NULL;
  append_asm_instr(&head, &tail, add_off);

  struct AsmInstr* store_instr = arena_alloc(sizeof(struct AsmInstr));
  store_instr->type = ASM_STORE;
  store_instr->dst = addr_temp;
  store_instr->src1 = src;
  store_instr->src2 = NULL;
  store_instr->next = NULL;
  append_asm_instr(&head, &tail, store_instr);

  return head;
}

struct AsmInstr* instr_to_asm(struct Slice* func_name, struct TACInstr* tac_instr) {
  if (tac_instr == NULL) {
    asm_gen_error0("instruction", func_name, "NULL TAC instruction encountered");
  }

  struct AsmInstr* asm_instr = arena_alloc(sizeof(struct AsmInstr));

  switch (tac_instr->type) {
    case TACRETURN:{
      struct TACReturn* ret_instr = &tac_instr->instr.tac_return;
      if (ret_instr->dst == NULL) {
        // TAC:
        // Return void
        //
        // ASM:
        // Ret
        asm_instr->type = ASM_RET;
        asm_instr->dst = NULL;
        asm_instr->src1 = NULL;
        asm_instr->src2 = NULL;
        asm_instr->next = NULL;
        return asm_instr;
      } else {
        bool return_in_memory = false;
        struct OperandList* ret_vars = NULL;
        classify_return_val(ret_instr->dst, &ret_vars, &return_in_memory);

        struct AsmInstr* head = NULL;
        struct AsmInstr* tail = NULL;

        if (return_in_memory) {
          struct AsmInstr* load_ret_ptr = arena_alloc(sizeof(struct AsmInstr));
          load_ret_ptr->type = ASM_MOV;
          load_ret_ptr->dst = arena_alloc(sizeof(struct Operand));
          load_ret_ptr->dst->type = OPERAND_REG;
          load_ret_ptr->dst->reg = kScratchRegA;
          load_ret_ptr->dst->asm_type = &kWordType;
          load_ret_ptr->src1 = make_asm_mem(BP, -4, &kWordType);
          load_ret_ptr->src2 = NULL;
          load_ret_ptr->next = NULL;
          append_asm_instr(&head, &tail, load_ret_ptr);

          struct Operand* dst_mem = make_asm_mem(kScratchRegA, 0, type_to_asm_type(ret_instr->dst->type));
          struct AsmInstr* copy_instrs = copy_bytes(func_name,
            tac_val_to_asm(ret_instr->dst),
            dst_mem,
            asm_type_size(type_to_asm_type(ret_instr->dst->type)));
          append_asm_instrs(&head, &tail, copy_instrs);
        } else {
          size_t reg_index = 0;
          for (struct OperandList* ret_iter = ret_vars; ret_iter != NULL; ret_iter = ret_iter->next) {
            struct Operand* ret_opr = ret_iter->opr;
            enum Reg ret_reg = (enum Reg)(R1 + reg_index);

            size_t ret_size = asm_type_size(ret_opr->asm_type);
            size_t ret_alignment = operand_base_alignment(ret_opr);
            bool needs_byte_copy = ret_opr->asm_type->type == BYTE_ARRAY ||
                                   ret_alignment < ret_size;

            if (needs_byte_copy) {
              struct AsmInstr* copy_instrs = copy_bytes_to_reg(func_name, ret_opr, ret_reg, ret_size);
              append_asm_instrs(&head, &tail, copy_instrs);
            } else {
              struct AsmInstr* mov_instr = arena_alloc(sizeof(struct AsmInstr));
              mov_instr->type = ASM_MOV;
              mov_instr->dst = arena_alloc(sizeof(struct Operand));
              mov_instr->dst->type = OPERAND_REG;
              mov_instr->dst->reg = ret_reg;
              mov_instr->dst->asm_type = ret_opr->asm_type;
              mov_instr->src1 = ret_opr;
              mov_instr->src2 = NULL;
              mov_instr->next = NULL;
              append_asm_instr(&head, &tail, mov_instr);
            }

            reg_index++;
          }
        }

        struct AsmInstr* ret_asm_instr = arena_alloc(sizeof(struct AsmInstr));
        ret_asm_instr->type = ASM_RET;
        ret_asm_instr->next = NULL;
        ret_asm_instr->dst = NULL;
        ret_asm_instr->src1 = NULL;
        ret_asm_instr->src2 = NULL;
        append_asm_instr(&head, &tail, ret_asm_instr);

        return head;
      }
    }
    case TACCOPY:{
      // TAC:
      // Copy dst, src
      //
      // ASM:
      // Mov dst, src
      struct TACCopy* copy_instr = &tac_instr->instr.tac_copy;
      struct Type* copy_type = copy_instr->dst->type != NULL ? copy_instr->dst->type : copy_instr->src->type;
      if (is_aggregate_type(copy_type)) {
        struct AsmInstr* copy_instrs = copy_bytes(func_name,
          tac_val_to_asm(copy_instr->src),
          tac_val_to_asm(copy_instr->dst),
          asm_type_size(type_to_asm_type(copy_type)));
        return copy_instrs;
      }
      asm_instr->type = ASM_MOV;
      asm_instr->dst = tac_val_to_asm(copy_instr->dst);
      asm_instr->src1 = tac_val_to_asm(copy_instr->src);
      asm_instr->src2 = NULL;
      asm_instr->next = NULL;
      return asm_instr;
    }
    case TACUNARY:{
      // TAC:
      // Unary op dst, src
      //
      // ASM:
      // Unary op dst, src
      struct TACUnary* unary_instr = &tac_instr->instr.tac_unary;
      asm_instr->type = ASM_UNARY;
      asm_instr->unary_op = unary_instr->op;
      asm_instr->dst = tac_val_to_asm(unary_instr->dst);
      asm_instr->src1 = tac_val_to_asm(unary_instr->src);
      asm_instr->src2 = NULL;
      asm_instr->next = NULL;
      return asm_instr;
    }
    case TACBINARY:{
      // TAC:
      // Binary op dst, src1, src2
      //
      // ASM:
      // Binary op dst, src1, src2
      struct TACBinary* binary_instr = &tac_instr->instr.tac_binary;
      asm_instr->type = ASM_BINARY;
      asm_instr->alu_op = binary_instr->alu_op;
      asm_instr->dst = tac_val_to_asm(binary_instr->dst);
      asm_instr->src1 = tac_val_to_asm(binary_instr->src1);
      asm_instr->src2 = tac_val_to_asm(binary_instr->src2);
      asm_instr->next = NULL;
      return asm_instr;
    }
    case TACCOND_JUMP:{
      // TAC:
      // CondJump cond, label
      //
      // ASM:
      // CondJump cond, label
      struct TACCondJump* cond_jump_instr = &tac_instr->instr.tac_cond_jump;
      asm_instr->type = ASM_COND_JUMP;
      asm_instr->cond = cond_jump_instr->condition;
      asm_instr->label = cond_jump_instr->label;
      asm_instr->dst = NULL;
      asm_instr->src1 = NULL;
      asm_instr->src2 = NULL;
      asm_instr->next = NULL;
      return asm_instr;
    }
    case TACCMP:{
      // TAC:
      // Cmp src1, src2
      //
      // ASM:
      // Cmp src1, src2
      struct TACCmp* cmp_instr = &tac_instr->instr.tac_cmp;
      asm_instr->type = ASM_CMP;
      asm_instr->dst = NULL;
      asm_instr->src1 = tac_val_to_asm(cmp_instr->src1);
      asm_instr->src2 = tac_val_to_asm(cmp_instr->src2);
      asm_instr->next = NULL;
      return asm_instr;
    }
    case TACJUMP:{
      // TAC:
      // Jump label
      //
      // ASM:
      // Jump label
      struct TACJump* jump_instr = &tac_instr->instr.tac_jump;
      asm_instr->type = ASM_JUMP;
      asm_instr->label = jump_instr->label;
      asm_instr->dst = NULL;
      asm_instr->src1 = NULL;
      asm_instr->src2 = NULL;
      asm_instr->next = NULL;
      return asm_instr;
    }
    case TACLABEL:{
      // TAC:
      // Label label
      //
      // ASM:
      // Label label
      struct TACLabel* label_instr = &tac_instr->instr.tac_label;
      asm_instr->type = ASM_LABEL;
      asm_instr->label = label_instr->label;
      asm_instr->dst = NULL;
      asm_instr->src1 = NULL;
      asm_instr->src2 = NULL;
      asm_instr->next = NULL;
      return asm_instr;
    }
    case TACCALL:
      return direct_call_to_asm(func_name, &tac_instr->instr.tac_call);
    case TACCALL_INDIRECT:
      return indirect_call_to_asm(func_name, &tac_instr->instr.tac_call_indirect);
    case TACGET_ADDRESS:{
      // TAC:
      // GetAddress dst, &src
      //
      // ASM:
      // GetAddress dst, &src
      struct TACGetAddress* get_addr_instr = &tac_instr->instr.tac_get_address;
      asm_instr->type = ASM_GET_ADDRESS;
      asm_instr->dst = tac_val_to_asm(get_addr_instr->dst);
      asm_instr->src1 = tac_val_to_asm(get_addr_instr->src);
      asm_instr->src2 = NULL;
      asm_instr->next = NULL;
      return asm_instr;
    }
    case TACLOAD:{
      // TAC:
      // Load dst, [ptr]
      //
      // ASM:
      // Load dst, [ptr]
      struct TACLoad* load_instr = &tac_instr->instr.tac_load;

      if (load_instr->dst->type->type == STRUCT_TYPE ||
          load_instr->dst->type->type == UNION_TYPE) {
        // aggregate load - dereference pointer then copy bytes into dst
        struct AsmInstr* head = NULL;
        struct AsmInstr* tail = NULL;

        struct Operand* ptr_reg = arena_alloc(sizeof(struct Operand));
        ptr_reg->type = OPERAND_REG;
        ptr_reg->reg = kScratchRegA;
        ptr_reg->asm_type = &kWordType;

        struct AsmInstr* load_ptr = arena_alloc(sizeof(struct AsmInstr));
        load_ptr->type = ASM_MOV;
        load_ptr->dst = ptr_reg;
        load_ptr->src1 = tac_val_to_asm(load_instr->src_ptr);
        load_ptr->src2 = NULL;
        load_ptr->next = NULL;
        append_asm_instr(&head, &tail, load_ptr);

        struct Operand* src_mem = make_asm_mem(kScratchRegA, 0,
          type_to_asm_type(load_instr->dst->type));
        struct AsmInstr* copy_instrs = copy_bytes(func_name,
          src_mem,
          tac_val_to_asm(load_instr->dst),
          asm_type_size(type_to_asm_type(load_instr->dst->type)));
        append_asm_instrs(&head, &tail, copy_instrs);

        return head;
      } else {
        // regular load
        asm_instr->type = ASM_LOAD;
        asm_instr->dst = tac_val_to_asm(load_instr->dst);
        asm_instr->src1 = tac_val_to_asm(load_instr->src_ptr);
        asm_instr->src2 = NULL;
        asm_instr->next = NULL;

        return asm_instr;
      }
    }
    case TACSTORE:{
      // TAC:
      // Store src, [ptr]
      //
      // ASM:
      // Store src, [ptr]
      struct TACStore* store_instr = &tac_instr->instr.tac_store;

      if (store_instr->src->type->type == STRUCT_TYPE ||
          store_instr->src->type->type == UNION_TYPE) {
        // aggregate store - dereference pointer then copy bytes from src
        struct AsmInstr* head = NULL;
        struct AsmInstr* tail = NULL;

        struct Operand* ptr_reg = arena_alloc(sizeof(struct Operand));
        ptr_reg->type = OPERAND_REG;
        ptr_reg->reg = kScratchRegA;
        ptr_reg->asm_type = &kWordType;

        struct AsmInstr* load_ptr = arena_alloc(sizeof(struct AsmInstr));
        load_ptr->type = ASM_MOV;
        load_ptr->dst = ptr_reg;
        load_ptr->src1 = tac_val_to_asm(store_instr->dst_ptr);
        load_ptr->src2 = NULL;
        load_ptr->next = NULL;
        append_asm_instr(&head, &tail, load_ptr);

        struct Operand* dst_mem = make_asm_mem(kScratchRegA, 0,
          type_to_asm_type(store_instr->src->type));
        struct AsmInstr* copy_instrs = copy_bytes(func_name,
          tac_val_to_asm(store_instr->src),
          dst_mem,
          asm_type_size(type_to_asm_type(store_instr->src->type)));
        append_asm_instrs(&head, &tail, copy_instrs);

        return head;
      } else {
        // regular store
        asm_instr->type = ASM_STORE;
        asm_instr->dst = tac_val_to_asm(store_instr->dst_ptr);
        asm_instr->src1 = tac_val_to_asm(store_instr->src);
        asm_instr->src2 = NULL;
        asm_instr->next = NULL;

        return asm_instr;
      }
    }
    case TACCOPY_TO_OFFSET:{
      struct TACCopyToOffset* copy_offset_instr = &tac_instr->instr.tac_copy_to_offset;
      struct Type* store_type = copy_offset_instr->dst_type != NULL
                                    ? copy_offset_instr->dst_type
                                    : copy_offset_instr->src->type;
      if (store_type == NULL) {
        asm_gen_error0("instruction", func_name,
                       "copy-to-offset missing type information for store");
      }

      if (store_type->type == STRUCT_TYPE ||
          store_type->type == UNION_TYPE) {
        // aggregate store - convert to copy of bytes
        struct AsmInstr* copy_instrs = copy_bytes(func_name, 
          tac_val_to_asm(copy_offset_instr->src),
          make_pseudo_mem(copy_offset_instr->dst, type_to_asm_type(store_type), copy_offset_instr->offset),
          asm_type_size(type_to_asm_type(store_type)));

        return copy_instrs;
      } else {
        if (copy_offset_instr->offset != 0 &&
            is_static_symbol_name(copy_offset_instr->dst)) {
          struct AsmSymbolEntry* base_entry =
              asm_symbol_table_get(asm_symbol_table, copy_offset_instr->dst);
          if (base_entry == NULL || base_entry->type == NULL) {
            asm_gen_error1_slice("instruction", func_name,
                                 "missing asm type for static base %.*s",
                                 copy_offset_instr->dst);
          }
          return store_to_static_offset_to_asm(func_name,
              copy_offset_instr->dst,
              base_entry->type,
              copy_offset_instr->offset,
              tac_val_to_asm(copy_offset_instr->src));
        }

        asm_instr->type = ASM_MOV;
        asm_instr->dst = make_pseudo_mem(copy_offset_instr->dst, type_to_asm_type(store_type), copy_offset_instr->offset);
        asm_instr->dst->asm_type = type_to_asm_type(store_type);
        asm_instr->src1 = tac_val_to_asm(copy_offset_instr->src);
        asm_instr->src2 = NULL;
        asm_instr->next = NULL;

        return asm_instr;
      }
    }
    case TACCOPY_FROM_OFFSET: {
      struct TACCopyFromOffset* copy_offset_instr = &tac_instr->instr.tac_copy_from_offset;
      struct Type* load_type = copy_offset_instr->dst->type;
      if (load_type == NULL) {
        asm_gen_error0("instruction", func_name,
                       "copy-to-offset missing type information for store");
      }

      if (load_type->type == STRUCT_TYPE ||
          load_type->type == UNION_TYPE) {
        // aggregate store - convert to copy of bytes
        struct AsmInstr* copy_instrs = copy_bytes(func_name, 
          make_pseudo_mem(copy_offset_instr->src, type_to_asm_type(load_type), copy_offset_instr->offset),
          tac_val_to_asm(copy_offset_instr->dst),
          asm_type_size(type_to_asm_type(load_type)));

        return copy_instrs;
      } else {
        if (copy_offset_instr->offset != 0 &&
            is_static_symbol_name(copy_offset_instr->src)) {
          struct AsmSymbolEntry* base_entry =
              asm_symbol_table_get(asm_symbol_table, copy_offset_instr->src);
          if (base_entry == NULL || base_entry->type == NULL) {
            asm_gen_error1_slice("instruction", func_name,
                                 "missing asm type for static base %.*s",
                                 copy_offset_instr->src);
          }
          return load_from_static_offset_to_asm(func_name,
              copy_offset_instr->src,
              base_entry->type,
              copy_offset_instr->offset,
              tac_val_to_asm(copy_offset_instr->dst));
        }

        asm_instr->type = ASM_MOV;
        asm_instr->dst = tac_val_to_asm(copy_offset_instr->dst);
        asm_instr->dst->asm_type = type_to_asm_type(load_type);
        asm_instr->src1 = make_pseudo_mem(copy_offset_instr->src, type_to_asm_type(load_type), copy_offset_instr->offset);
        asm_instr->src2 = NULL;
        asm_instr->next = NULL;

        return asm_instr;
      }
    }
    case TACBOUNDARY: {
      // TAC:
      // Line marker
      //
      // ASM:
      // Line marker
      asm_instr->type = ASM_BOUNDARY;
      asm_instr->loc = tac_instr->instr.tac_boundary.loc;
      asm_instr->dst = NULL;
      asm_instr->src1 = NULL;
      asm_instr->src2 = NULL;
      asm_instr->next = NULL;
      return asm_instr;
    }
    case TACTRUNC: {
      asm_instr->type = ASM_TRUNC;
      asm_instr->dst = tac_val_to_asm(tac_instr->instr.tac_trunc.dst);
      asm_instr->src1 = tac_val_to_asm(tac_instr->instr.tac_trunc.src);
      asm_instr->size = tac_instr->instr.tac_trunc.target_size;
      asm_instr->src2 = NULL;
      asm_instr->next = NULL;
      return asm_instr;
    }
    case TACEXTEND: {
      asm_instr->type = ASM_EXTEND;
      asm_instr->dst = tac_val_to_asm(tac_instr->instr.tac_extend.dst);
      asm_instr->src1 = tac_val_to_asm(tac_instr->instr.tac_extend.src);
      asm_instr->size = tac_instr->instr.tac_extend.src_size;
      asm_instr->src2 = NULL;
      asm_instr->next = NULL;
      return asm_instr;
    }
    default:
      asm_gen_error2_int_str("instruction", func_name,
                             "unknown TAC instruction type %d (%s)",
                             (int)tac_instr->type,
                             tac_instr_name(tac_instr->type));
      return NULL;
  }

  return asm_instr;
}

size_t create_maps(struct AsmInstr* asm_instr, size_t reserved_bytes) {
  if (pseudo_map != NULL) {
    asm_gen_error0("stack-map", NULL, "pseudo map already initialized");
  }

  pseudo_map = create_pseudo_map(128);
  size_t stack_bytes = reserved_bytes;

  for (struct AsmInstr* instr = asm_instr; instr != NULL; instr = instr->next) {
    size_t op_count = 0;
    struct Operand** ops = get_ops(instr, &op_count);
    if (ops == NULL) {
      continue;
    }

    for (size_t i = 0; i < op_count; i++) {
      struct Operand* opr = ops[i];
      if (opr == NULL) {
        continue;
      }

      if (opr->type != OPERAND_PSEUDO && opr->type != OPERAND_PSEUDO_MEM) {
        continue;
      }

      if (pseudo_map_get(pseudo_map, opr) != NULL) {
        continue;
      }

      struct AsmSymbolEntry* sym_entry = asm_symbol_table_get(asm_symbol_table, opr->pseudo);
      if (sym_entry == NULL) {
        asm_gen_error1_slice("stack-map", NULL,
                             "missing symbol table entry for pseudo %.*s",
                             opr->pseudo);
      }

      struct Operand* mapped = arena_alloc(sizeof(struct Operand));
      if (is_static_symbol_operand(opr)) {
        mapped->type = OPERAND_DATA;
        mapped->reg = 0;
        mapped->lit_value = 0;
        mapped->pseudo = opr->pseudo;
        mapped->asm_type = sym_entry->type;
      } else {
        mapped->type = OPERAND_MEMORY;
        mapped->reg = BP;
        mapped->lit_value = allocate_stack_slot(opr, &stack_bytes);
        // Offset is applied at lookup time so each pseudo-mem use keeps its own byte offset.
        mapped->pseudo = NULL;
        mapped->asm_type = sym_entry->type;
      }

      pseudo_map_insert(pseudo_map, opr, mapped);
    }
  }

  // pad to 4-byte alignment
  size_t padding = (4 - (stack_bytes % 4)) % 4;
  stack_bytes += padding;

  return stack_bytes;
}

struct Operand** get_ops(struct AsmInstr* asm_instr, size_t* out_count) {
  size_t src_count = 0;
  struct Operand** srcs = get_srcs(asm_instr, &src_count);
  struct Operand* dst = get_dst(asm_instr);

  size_t total_count = src_count + (dst != NULL ? 1 : 0);
  struct Operand** ops = arena_alloc(total_count * sizeof(struct Operand*));

  size_t index = 0;
  if (dst != NULL) {
    ops[index++] = dst;
  }
  for (size_t i = 0; i < src_count; i++) {
    ops[index++] = srcs[i];
  }

  *out_count = total_count;
  return ops;
}

struct Operand** get_srcs(struct AsmInstr* asm_instr, size_t* out_count) {
  switch (asm_instr->type) {
    case ASM_MOV:
      *out_count = 1;
      struct Operand** srcs_mov = arena_alloc(sizeof(struct Operand*));
      srcs_mov[0] = asm_instr->src1;
      return srcs_mov;
    case ASM_UNARY:
      *out_count = 1;
      struct Operand** srcs_unary = arena_alloc(sizeof(struct Operand*));
      srcs_unary[0] = asm_instr->src1;
      return srcs_unary;
    case ASM_BINARY:
      *out_count = 2;
      struct Operand** srcs_binary = arena_alloc(2 * sizeof(struct Operand*));
      srcs_binary[0] = asm_instr->src1;
      srcs_binary[1] = asm_instr->src2;
      return srcs_binary;
    case ASM_CMP:
      *out_count = 2;
      struct Operand** srcs_cmp = arena_alloc(2 * sizeof(struct Operand*));
      srcs_cmp[0] = asm_instr->src1;
      srcs_cmp[1] = asm_instr->src2;
      return srcs_cmp;
    case ASM_PUSH:
      *out_count = 1;
      struct Operand** srcs_push = arena_alloc(sizeof(struct Operand*));
      srcs_push[0] = asm_instr->src1;
      return srcs_push;
    case ASM_GET_ADDRESS:
      *out_count = 1;
      struct Operand** srcs_getaddr = arena_alloc(sizeof(struct Operand*));
      srcs_getaddr[0] = asm_instr->src1;
      return srcs_getaddr;
    case ASM_LOAD:
      *out_count = 1;
      struct Operand** srcs_load = arena_alloc(sizeof(struct Operand*));
      srcs_load[0] = asm_instr->src1;
      return srcs_load;
    case ASM_STORE:
      *out_count = 2;
      struct Operand** srcs_store = arena_alloc(2 * sizeof(struct Operand*));
      srcs_store[0] = asm_instr->src1;
      // Store uses dst as the address operand.
      srcs_store[1] = asm_instr->dst;
      return srcs_store;
    case ASM_TRUNC:
      *out_count = 1;
      struct Operand** srcs_trunc = arena_alloc(sizeof(struct Operand*));
      srcs_trunc[0] = asm_instr->src1;
      return srcs_trunc;
    case ASM_EXTEND:
      *out_count = 1;
      struct Operand** srcs_extend = arena_alloc(sizeof(struct Operand*));
      srcs_extend[0] = asm_instr->src1;
      return srcs_extend;
    case ASM_INDIRECT_CALL:
      *out_count = 1;
      struct Operand** srcs_indirect_call = arena_alloc(sizeof(struct Operand*));
      srcs_indirect_call[0] = asm_instr->src1;
      return srcs_indirect_call;
    default:
      *out_count = 0;
      return NULL;
  }
}

struct Operand* get_dst(struct AsmInstr* asm_instr) {
  switch (asm_instr->type) {
    case ASM_MOV:
      return asm_instr->dst;
    case ASM_UNARY:
      return asm_instr->dst;
    case ASM_BINARY:
      return asm_instr->dst;
    case ASM_GET_ADDRESS:
      return asm_instr->dst;
    case ASM_LOAD:
      return asm_instr->dst;
    case ASM_TRUNC:
      return asm_instr->dst;
    case ASM_EXTEND:
      return asm_instr->dst;
    default:
      return NULL;
  }
}

void replace_pseudo(struct AsmInstr* asm_instr) {
  for (struct AsmInstr* instr = asm_instr; instr != NULL; instr = instr->next) {
    replace_operand_if_pseudo(&instr->dst);
    replace_operand_if_pseudo(&instr->src1);
    replace_operand_if_pseudo(&instr->src2);
  }
}

bool is_static_symbol_operand(struct Operand* opr) {
  if (opr == NULL || opr->pseudo == NULL || global_symbol_table == NULL) {
    return false;
  }
  struct SymbolEntry* entry = symbol_table_get(global_symbol_table, opr->pseudo);
  if (entry == NULL || entry->attrs == NULL) {
    return false;
  }
  if (entry->type != NULL && entry->type->type == FUN_TYPE) {
    // Function symbols live in the text segment and should be treated as static operands.
    return true;
  }
  return entry->attrs->attr_type == STATIC_ATTR ||
         entry->attrs->attr_type == CONST_ATTR;
}

int allocate_stack_slot(struct Operand* opr, size_t* stack_bytes) {
  if (opr == NULL) {
    asm_gen_error0("stack-map", NULL,
                   "NULL operand passed to allocate_stack_slot");
  }
  if (opr->type != OPERAND_PSEUDO && opr->type != OPERAND_PSEUDO_MEM) {
    asm_gen_error1_int("stack-map", NULL,
                       "unexpected operand type %d for stack slot allocation",
                       (int)opr->type);
  }

  struct AsmSymbolEntry* sym_entry = asm_symbol_table_get(asm_symbol_table, opr->pseudo);
  if (sym_entry == NULL) {
    asm_gen_error1_slice("stack-map", NULL,
                         "missing symbol table entry for pseudo %.*s",
                         opr->pseudo);
  }
  // add padding if necessary for alignment
  size_t alignment = asm_type_alignment(sym_entry->type);
  size_t padding = (alignment - (*stack_bytes % alignment)) % alignment;
  *stack_bytes += padding;

  size_t next_size = *stack_bytes + asm_type_size(sym_entry->type);
  *stack_bytes = next_size;
  return -((int)next_size);
}

void replace_operand_if_pseudo(struct Operand** field) {
  if (field == NULL || *field == NULL) {
    return;
  }

  if (pseudo_map == NULL) {
    asm_gen_error0("stack-map", NULL,
                   "pseudo map not initialized before replacement");
  }

  struct Operand* mapped = pseudo_map_get(pseudo_map, *field);
  if (mapped != NULL) {
    *field = mapped;
    return;
  }

  if ((*field)->type == OPERAND_PSEUDO || (*field)->type == OPERAND_PSEUDO_MEM) {
    size_t len = 0;
    char* name = "<unknown>";
    if ((*field)->pseudo != NULL) {
        name = (*field)->pseudo->start;
        len = (*field)->pseudo->len;
    }
    asm_gen_error1_len_str("stack-map", NULL,
                           "missing mapping for pseudo %.*s", (int)len, name);
  }
}

struct Operand* tac_val_to_asm(struct Val* val) {
  if (val == NULL) {
    asm_gen_error0("operand", NULL, "NULL TAC value encountered");
  }

  struct Operand* opr = arena_alloc(sizeof(struct Operand));

  switch (val->val_type) {
    case CONSTANT: {
      opr->type = OPERAND_LIT;
      opr->lit_value = (int)(val->val.const_value); // assuming fits in int
      opr->asm_type = type_to_asm_type(val->type);
      return opr;
    }
    case VARIABLE: {
      switch (val->type->type) {
        case CHAR_TYPE:
        case SCHAR_TYPE:
        case UCHAR_TYPE:
        case SHORT_TYPE:
        case USHORT_TYPE:
        case INT_TYPE:
        case UINT_TYPE:
        case LONG_TYPE:
        case ULONG_TYPE:
        case ENUM_TYPE:
        case POINTER_TYPE: {
          // Pseudo Operand for scalars
          opr->type = OPERAND_PSEUDO;
          opr->pseudo = val->val.var_name;
          opr->asm_type = type_to_asm_type(val->type);
          return opr;
        }
        case FUN_TYPE: {
          // Function designators are treated as pointer-sized pseudos.
          opr->type = OPERAND_PSEUDO;
          opr->pseudo = val->val.var_name;
          opr->asm_type = &kWordType;
          return opr;
        }
        case STRUCT_TYPE:
        case UNION_TYPE: 
        case ARRAY_TYPE: {
          // PseudoMem Operand for arrays, structs, and unions
          opr->type = OPERAND_PSEUDO_MEM;
          opr->pseudo = val->val.var_name;
          opr->lit_value = 0; // offset 0 for now
          opr->asm_type = type_to_asm_type(val->type);
          return opr;
        }
        default:
          asm_gen_error1_int("operand", NULL,
                             "unsupported variable type %d for TAC to ASM conversion",
                             (int)val->type->type);
          return NULL;
      }
    }
    default:
      asm_gen_error1_int("operand", NULL,
                         "unknown TAC value type %d", (int)val->val_type);
      return NULL;
  }
}

struct Operand* make_pseudo(struct Slice* var_name, struct AsmType* asm_type) {
  if (var_name == NULL) {
    asm_gen_error0("operand", NULL, "NULL slice for pseudo operand");
  }

  struct Operand* opr = arena_alloc(sizeof(struct Operand));
  opr->type = OPERAND_PSEUDO;
  opr->pseudo = var_name;
  opr->asm_type = asm_type;
  return opr;
}

struct Operand* make_pseudo_mem(struct Slice* var_name, struct AsmType* asm_type, int offset) {
  if (var_name == NULL) {
    asm_gen_error0("operand", NULL, "NULL slice for pseudo-mem operand");
  }

  struct Operand* opr = arena_alloc(sizeof(struct Operand));
  opr->type = OPERAND_PSEUDO_MEM;
  opr->pseudo = var_name;
  opr->lit_value = offset;
  opr->asm_type = asm_type;
  return opr;
}

size_t type_alignment(struct Type* type, struct Slice* symbol_name) {
  // will eventually have different alignments for different types
  // short => 2, char => 1
  if (type == NULL) {
    asm_gen_error0("type-alignment", symbol_name, "NULL type for static symbol");
  }
  switch (type->type) {
    case CHAR_TYPE:
    case SCHAR_TYPE:
    case UCHAR_TYPE:
      return 1;
    case SHORT_TYPE:
    case USHORT_TYPE:
      return 2;
    case INT_TYPE:
    case UINT_TYPE:
    case LONG_TYPE:
    case ULONG_TYPE:
    case POINTER_TYPE:
      return 4;
    case ENUM_TYPE:
      return 4;
    case ARRAY_TYPE:
      return type_alignment(type->type_data.array_type.element_type, symbol_name);
    case STRUCT_TYPE:
    case UNION_TYPE:
      return get_type_alignment(type);
    default:
      asm_gen_error1_int("type-alignment", symbol_name,
                         "unknown type kind %d", (int)type->type);
      return 0;
  }
}

// Purpose: Resolve the type table entry for a struct or union type.
// Inputs: type is the aggregate type to resolve.
// Outputs: Returns the StructEntry for the aggregate.
// Invariants/Assumptions: type is STRUCT_TYPE or UNION_TYPE.
static struct StructEntry* get_aggregate_entry(struct Type* type) {
  if (type == NULL) {
    asm_gen_error0("aggregate", NULL, "NULL type for aggregate lookup");
  }
  struct TypeEntry* entry = NULL;
  if (type->type == STRUCT_TYPE) {
    entry = type_table_get(global_type_table, type->type_data.struct_type.name);
  } else if (type->type == UNION_TYPE) {
    entry = type_table_get(global_type_table, type->type_data.union_type.name);
  } else {
    asm_gen_error1_int("aggregate", NULL,
                       "non-aggregate type %d for lookup", (int)type->type);
  }
  if (entry == NULL) {
    asm_gen_error0("aggregate", NULL, "missing type entry for aggregate");
  }
  return (entry->type == STRUCT_ENTRY) ? entry->data.struct_entry : entry->data.union_entry;
}

struct VarClassList* classify_struct(struct StructEntry* struct_entry) {
  if (struct_entry == NULL) {
    asm_gen_error0("struct-classify", NULL, "NULL struct entry");
  }
  int size = struct_entry->size;

  if (size > 8){
    // too large to fit in 2 registers, so pass in memory
    struct VarClassList* list = arena_alloc(sizeof(struct VarClassList));
    struct VarClassList* tail = list;
    list->var_class = MEMORY_CLASS;
    list->next = NULL;
    size -= 4;

    while (size > 0){
      struct VarClassList* next = arena_alloc(sizeof(struct VarClassList));
      next->var_class = MEMORY_CLASS;
      next->next = NULL;
      tail->next = next;
      tail = next;
      size -=4;
    }
    return list;
  }

  // small enough to fit in registers

  if (size > 4){
    // fits in 2 registers
    struct VarClassList* list = arena_alloc(sizeof(struct VarClassList));
    list->var_class = INTEGER_CLASS;
    list->next = arena_alloc(sizeof(struct VarClassList));
    list->next->var_class = INTEGER_CLASS;
    list->next->next = NULL;
    return list;
  }
  
  // fits in 1 register
  struct VarClassList* list = arena_alloc(sizeof(struct VarClassList));
  list->var_class = INTEGER_CLASS;
  list->next = NULL;
  return list;
}

struct AsmType* get_fourbyte_type(size_t offset, size_t struct_size){
  if (struct_size - offset >= 4){
    struct AsmType* word = arena_alloc(sizeof(struct AsmType));
    word->type = WORD;
    return word;
  } else if (struct_size - offset == 2){
    struct AsmType* double_ = arena_alloc(sizeof(struct AsmType));
    double_->type = DOUBLE;
    return double_;
  } else if (struct_size - offset == 1){
    struct AsmType* byte = arena_alloc(sizeof(struct AsmType));
    byte->type = BYTE;
    return byte;
  } else {
    // return byte for any other size (e.g., 3 bytes)
    struct AsmType* byte_array = arena_alloc(sizeof(struct AsmType));
    byte_array->type = BYTE_ARRAY;
    byte_array->byte_array.size = struct_size - offset;
    byte_array->byte_array.alignment = 1;
    return byte_array;
  }
}

void classify_params(struct Val* params, size_t num_params, bool return_in_memory,
                     struct OperandList** reg_args, struct OperandList** stack_args) {
  *reg_args = NULL;
  *stack_args = NULL;

  size_t regs_available = 0;
  if (return_in_memory){
    // first reg is pointer to return value memory
    regs_available = REG_ARG_LIMIT - 1;
  } else {
    regs_available = REG_ARG_LIMIT;
  }

  struct OperandList* reg_tail = NULL;
  struct OperandList* stack_tail = NULL;

  for (size_t i = 0; i < num_params; i++){
    struct Type* param_type = params[i].type;

    struct Operand* param_opr = tac_val_to_asm(&params[i]);

    if (param_type->type == STRUCT_TYPE ||
        param_type->type == UNION_TYPE) {
      // partition struct/union into fourbytes by class
      struct StructEntry* struct_entry = get_aggregate_entry(param_type);
      struct VarClassList* class_list = classify_struct(struct_entry);

      bool use_stack = true;
      size_t struct_size = struct_entry->size;

      if (class_list->var_class != MEMORY_CLASS){
        // make tentative assignment to registers
        struct OperandList* tentative_regs = NULL;
        struct OperandList* tentative_tail = NULL;
        size_t offset = 0;
        size_t regs_needed = 0;
        for (struct VarClassList* cls = class_list; cls != NULL; cls = cls->next){
          struct Operand* opr = arena_alloc(sizeof(struct Operand));
          opr->type = OPERAND_PSEUDO_MEM;
          opr->pseudo = param_opr->pseudo;
          opr->lit_value = offset;
          opr->asm_type = get_fourbyte_type(offset, struct_size);

          struct OperandList* entry = arena_alloc(sizeof(struct OperandList));
          entry->opr = opr;
          entry->next = NULL;
          if (tentative_regs == NULL) {
            tentative_regs = entry;
            tentative_tail = entry;
          } else {
            tentative_tail->next = entry;
            tentative_tail = entry;
          }

          offset += 4;
          regs_needed++;
        }

        // finalize assignments if enough registers available
        if (regs_needed <= regs_available){
          use_stack = false;
          regs_available -= regs_needed;

          // add tentative regs to reg_args
          if (*reg_args == NULL){
            *reg_args = tentative_regs;
            reg_tail = tentative_tail;
          } else {
            reg_tail->next = tentative_regs;
            // update reg_tail to end of tentative_regs
            while (reg_tail->next != NULL){
              reg_tail = reg_tail->next;
            }
          }
        }
      }

      if (use_stack){
        // assign entire struct to stack
        
        size_t offset = 0;
        for (struct VarClassList* cls = class_list; cls != NULL; cls = cls->next){
          struct Operand* opr = arena_alloc(sizeof(struct Operand));
          opr->type = OPERAND_PSEUDO_MEM;
          opr->pseudo = param_opr->pseudo;
          opr->lit_value = offset;
          opr->asm_type = get_fourbyte_type(offset, struct_size);

          offset += 4;

          struct OperandList* stack_entry = arena_alloc(sizeof(struct OperandList));
          stack_entry->opr = opr;
          stack_entry->next = NULL;

          if (*stack_args == NULL){
            *stack_args = stack_entry;
            stack_tail = stack_entry;
          } else {
            stack_tail->next = stack_entry;
            stack_tail = stack_entry;
          }
        }
      }
    } else {
      // scalar type, assign to register if available, stack if not
      if (regs_available > 0){
        // assign to register
        struct OperandList* reg_entry = arena_alloc(sizeof(struct OperandList));
        reg_entry->opr = param_opr;
        reg_entry->next = NULL;

        if (*reg_args == NULL){
          *reg_args = reg_entry;
          reg_tail = reg_entry;
        } else {
          reg_tail->next = reg_entry;
          reg_tail = reg_entry;
        }

        regs_available--;
      } else {
        // assign to stack
        struct OperandList* stack_entry = arena_alloc(sizeof(struct OperandList));
        stack_entry->opr = param_opr;
        stack_entry->next = NULL;

        if (*stack_args == NULL){
          *stack_args = stack_entry;
          stack_tail = stack_entry;
        } else {
          stack_tail->next = stack_entry;
          stack_tail = stack_entry;
        }
      }
    }
  }
}

void classify_return_val(struct Val* ret_val, struct OperandList** ret_var_list, bool* return_in_memory) {
  if (ret_val == NULL) {
    asm_gen_error0("return-classify", NULL, "NULL return value");
  }

  struct AsmType* ret_type = type_to_asm_type(ret_val->type);

  if (ret_type->type == BYTE_ARRAY) {
    // aggregate return, may or may not fit in registers
    struct StructEntry* struct_entry = get_aggregate_entry(ret_val->type);
    struct VarClassList* class_list = classify_struct(struct_entry);
    size_t size = struct_entry->size;
    
    if (class_list->var_class == MEMORY_CLASS) {
      // return in memory
      *return_in_memory = true;
      *ret_var_list = NULL;
      return;
    } else {
      // return in registers
      *return_in_memory = false;
      struct OperandList* ret_entry = NULL;
      struct OperandList* ret_tail = NULL;
      size_t offset = 0;

      for (struct VarClassList* cls = class_list; cls != NULL; cls = cls->next){
        struct Operand* opr = arena_alloc(sizeof(struct Operand));
        opr->type = OPERAND_PSEUDO_MEM;
        opr->pseudo = ret_val->val.var_name;
        opr->lit_value = offset;
        opr->asm_type = get_fourbyte_type(offset, size);

        offset += 4;

        struct OperandList* entry = arena_alloc(sizeof(struct OperandList));
        entry->opr = opr;
        entry->next = NULL;

        if (ret_entry == NULL){
          ret_entry = entry;
          ret_tail = entry;
        } else {
          ret_tail->next = entry;
          ret_tail = entry;
        }
      }

      *ret_var_list = ret_entry;
      return;
    }
  } else {
    // scalar return, fits in register
    struct Operand* ret_opr = tac_val_to_asm(ret_val);
    struct OperandList* ret_entry = arena_alloc(sizeof(struct OperandList));
    ret_entry->opr = ret_opr;
    ret_entry->next = NULL;
    *ret_var_list = ret_entry;
    *return_in_memory = false;
    return;
  }
}

// Purpose: Lower parameter passing for a function using pre-built Val entries.
// Inputs: func_name identifies the owning function; params is the parameter Val array.
// Outputs: Returns the head of the ASM instruction list setting up parameters.
// Invariants/Assumptions: params entries have valid types and variable names.
static struct AsmInstr* set_up_params_from_vals(struct Slice* func_name,
                                                struct Val* params,
                                                size_t num_params,
                                                bool return_in_memory) {
  struct OperandList* reg_param_list = NULL;
  struct OperandList* stack_param_list = NULL;
  classify_params(params, num_params, return_in_memory, &reg_param_list, &stack_param_list);

  struct AsmInstr* head = NULL;
  struct AsmInstr* tail = NULL;
  size_t reg_index = 0;

  if (return_in_memory){
    // first param is pointer to return value memory
    struct AsmInstr* param_instr = arena_alloc(sizeof(struct AsmInstr));
    param_instr->type = ASM_MOV;
    param_instr->dst = arena_alloc(sizeof(struct Operand));
    param_instr->dst->type = OPERAND_MEMORY;
    param_instr->dst->reg = BP;
    param_instr->dst->lit_value = -4; // first stack slot
    param_instr->dst->asm_type = &kWordType;
    param_instr->src1 = arena_alloc(sizeof(struct Operand));
    param_instr->src1->type = OPERAND_REG;
    param_instr->src1->reg = R1;
    param_instr->src1->asm_type = &kWordType;
    param_instr->src2 = NULL;
    param_instr->next = NULL;
    append_asm_instr(&head, &tail, param_instr);

    reg_index = 1;
  }

  // set up register params
  for (struct OperandList* reg_param = reg_param_list; reg_param != NULL; reg_param = reg_param->next){
    size_t param_size = asm_type_size(reg_param->opr->asm_type);
    size_t param_alignment = operand_base_alignment(reg_param->opr);
    bool needs_byte_copy = reg_param->opr->asm_type->type == BYTE_ARRAY ||
                           param_alignment < param_size;
    if (needs_byte_copy){
      struct AsmInstr* copy_instrs = copy_bytes_from_reg(func_name,
        R1 + reg_index,
        reg_param->opr,
        param_size);
      append_asm_instrs(&head, &tail, copy_instrs);
    } else {
      struct AsmInstr* param_instr = arena_alloc(sizeof(struct AsmInstr));
      param_instr->type = ASM_MOV;
      param_instr->src1 = arena_alloc(sizeof(struct Operand));
      param_instr->src1->type = OPERAND_REG;
      param_instr->src1->reg = R1 + reg_index;
      param_instr->dst = reg_param->opr;
      param_instr->src2 = NULL;
      param_instr->next = NULL;
      append_asm_instr(&head, &tail, param_instr);
    }

    reg_index++;
  }

  size_t offset = 8; // first stack param offset
  for (struct OperandList* stack_param = stack_param_list; stack_param != NULL; stack_param = stack_param->next){
    size_t param_size = asm_type_size(stack_param->opr->asm_type);
    size_t param_alignment = operand_base_alignment(stack_param->opr);
    bool needs_byte_copy = stack_param->opr->asm_type->type == BYTE_ARRAY ||
                           param_alignment < param_size;
    if (needs_byte_copy){
      struct AsmInstr* copy_instrs = copy_bytes(func_name,
        make_asm_mem(BP, offset, stack_param->opr->asm_type),
        stack_param->opr,
        param_size);
      append_asm_instrs(&head, &tail, copy_instrs);
    } else {
      struct AsmInstr* param_instr = arena_alloc(sizeof(struct AsmInstr));
      param_instr->type = ASM_MOV;
      param_instr->dst = stack_param->opr;
      param_instr->src1 = make_asm_mem(BP, offset, stack_param->opr->asm_type);
      param_instr->src2 = NULL;
      param_instr->next = NULL;
      append_asm_instr(&head, &tail, param_instr);
    }

    offset += 4;
  }

  return head;
}

// Purpose: Lower parameter passing for a function definition.
// Inputs: func_name identifies the owning function; params are parameter names.
// Outputs: Returns the head of the ASM instruction list setting up parameters.
// Invariants/Assumptions: params are declared in the global symbol table.
struct AsmInstr* set_up_params(struct Slice* func_name,
                               struct Slice** params,
                               size_t num_params,
                               bool return_in_memory) {
  if (num_params == 0) {
    return set_up_params_from_vals(func_name, NULL, 0, return_in_memory);
  }

  struct Val* param_vals = arena_alloc(num_params * sizeof(struct Val));
  for (size_t i = 0; i < num_params; i++) {
    struct SymbolEntry* entry = symbol_table_get(global_symbol_table, params[i]);
    if (entry == NULL || entry->type == NULL) {
      asm_gen_error1_slice("params", func_name,
                           "missing type info for parameter %.*s",
                           params[i]);
    }
    param_vals[i].val_type = VARIABLE;
    param_vals[i].val.var_name = params[i];
    param_vals[i].type = entry->type;
  }

  return set_up_params_from_vals(func_name, param_vals, num_params, return_in_memory);
}

size_t asm_type_alignment(struct AsmType* type){
  if (type == NULL) {
    asm_gen_error0("asm-type-alignment", NULL, "NULL asm type");
  }
  switch (type->type) {
    case BYTE:
      return 1;
    case DOUBLE:
      return 2;
    case WORD:
    case LONG_WORD:
      return 4;
    case BYTE_ARRAY:
      return type->byte_array.alignment;
    default:
      asm_gen_error1_int("asm-type-alignment", NULL,
                         "unknown asm type kind %d", (int)type->type);
      return 0;
  }
}

struct PseudoMap* create_pseudo_map(size_t num_buckets){
  struct PseudoEntry** arr = malloc(num_buckets * sizeof(struct PseudoEntry*));
  struct PseudoMap* hmap = malloc(sizeof(struct PseudoMap));

  if (arr == NULL || hmap == NULL) {
    free(arr);
    free(hmap);
    asm_gen_error0("stack-map", NULL, "allocation failed for pseudo map");
  }

  for (size_t i = 0; i < num_buckets; ++i){
    arr[i] = NULL;
  }

  hmap->size = num_buckets;
  hmap->arr = arr;

  return hmap;
}


struct PseudoEntry* create_pseudo_entry(struct Operand* key, struct Operand* value){
  struct PseudoEntry* entry = malloc(sizeof(struct PseudoEntry));

  entry->pseudo = key;
  entry->mapped = value;
  entry->next = NULL;

  return entry;
}

void pseudo_entry_insert(struct PseudoEntry* entry, struct Operand* key, struct Operand* value){
  if (compare_slice_to_slice(entry->pseudo->pseudo, key->pseudo)){
    entry->mapped = value;
  } else if (entry->next == NULL){
    entry->next = create_pseudo_entry(key, value);
  } else {
    pseudo_entry_insert(entry->next, key, value);
  }
}


void pseudo_map_insert(struct PseudoMap* hmap, struct Operand* key, struct Operand* value){
  if (hmap == NULL || key == NULL || key->pseudo == NULL) {
    asm_gen_error0("stack-map", NULL, "invalid pseudo map insert request");
  }
  size_t label = hash_slice(key->pseudo) % hmap->size;
  
  if ((hmap->arr[label]) == NULL){
    hmap->arr[label] = create_pseudo_entry(key, value);
  } else {
    pseudo_entry_insert(hmap->arr[label], key, value);
  }
}

struct Operand* pseudo_entry_get(struct PseudoEntry* entry, struct Operand* key){
  if (compare_slice_to_slice(entry->pseudo->pseudo, key->pseudo)){
    return entry->mapped;
  } else if (entry->next == NULL){
    return 0;
  } else {
    return pseudo_entry_get(entry->next, key);
  }
}

struct Operand* pseudo_map_get(struct PseudoMap* hmap, struct Operand* key){
  if (hmap == NULL || key == NULL) {
    asm_gen_error0("stack-map", NULL, "invalid pseudo map lookup request");
  }
  if (key->type != OPERAND_PSEUDO && key->type != OPERAND_PSEUDO_MEM) {
    // not a pseudo operand, do not replace
    return NULL;
  }
  if (key->pseudo == NULL) {
    asm_gen_error0("stack-map", NULL, "pseudo operand missing identifier");
  }
  
  size_t label = hash_slice(key->pseudo) % hmap->size;

  if (hmap->arr[label] == NULL){
    return 0;
  } else {
    struct Operand* mapped = pseudo_entry_get(hmap->arr[label], key);
    if (mapped == NULL) {
      return NULL;
    }
    if (key->type == OPERAND_PSEUDO_MEM) {
      struct Operand* derived = arena_alloc(sizeof(struct Operand));
      *derived = *mapped;
      derived->lit_value = mapped->lit_value + key->lit_value;
      derived->asm_type = key->asm_type;
      return derived;
    }
    return mapped;
  }
}


bool pseudo_entry_contains(struct PseudoEntry* entry, struct Operand* key){
  if (compare_slice_to_slice(entry->pseudo->pseudo, key->pseudo)){
    return true;
  } else if (entry->next == NULL){
    return false;
  } else {
    return pseudo_entry_contains(entry->next, key);
  }
}

bool pseudo_map_contains(struct PseudoMap* hmap, struct Operand* key){
  if (hmap == NULL || key == NULL || key->pseudo == NULL) {
    asm_gen_error0("stack-map", NULL, "invalid pseudo map contains request");
  }
  size_t label = hash_slice(key->pseudo) % hmap->size;

  if (hmap->arr[label] == NULL){
    return false;
  } else {
    return pseudo_entry_contains(hmap->arr[label], key);
  }
}

void destroy_pseudo_entry(struct PseudoEntry* entry){
  if (entry->next != NULL) destroy_pseudo_entry(entry->next);
  free(entry);
}

void destroy_pseudo_map(struct PseudoMap* hmap){
  for (int i = 0; i < hmap->size; ++i){
    if (hmap->arr[i] != NULL) destroy_pseudo_entry(hmap->arr[i]);
  }
  free(hmap->arr);
  free(hmap);
}

struct AsmSymbolTable* create_asm_symbol_table(size_t numBuckets){
  struct AsmSymbolTable* table = arena_alloc(sizeof(struct AsmSymbolTable));
  table->size = numBuckets;
  table->arr = arena_alloc(sizeof(struct AsmSymbolEntry*) * numBuckets);
  for (size_t i = 0; i < numBuckets; i++){
    table->arr[i] = NULL;
  }
  return table;
}

void asm_symbol_table_insert(struct AsmSymbolTable* hmap, struct Slice* key, struct AsmType* type, 
    bool is_static, bool is_defined, bool return_on_stack){
  size_t label = hash_slice(key) % hmap->size;
  
  struct AsmSymbolEntry* newEntry = arena_alloc(sizeof(struct AsmSymbolEntry));
  newEntry->key = key;
  newEntry->type = type;
  newEntry->is_static = is_static;
  newEntry->is_defined = is_defined;
  newEntry->return_on_stack = return_on_stack;
  newEntry->next = NULL;

  if (hmap->arr[label] == NULL){
    hmap->arr[label] = newEntry;
  } else {
    struct AsmSymbolEntry* cur = hmap->arr[label];
    while (cur->next != NULL){
      cur = cur->next;
    }
    cur->next = newEntry;
  }
}

struct AsmSymbolEntry* asm_symbol_table_get(struct AsmSymbolTable* hmap, struct Slice* key){
  size_t label = hash_slice(key) % hmap->size;

  struct AsmSymbolEntry* cur = hmap->arr[label];
  while (cur != NULL){
    if (compare_slice_to_slice(cur->key, key)){
      return cur;
    }
    cur = cur->next;
  }
  return NULL;
}

bool asm_symbol_table_contains(struct AsmSymbolTable* hmap, struct Slice* key){
  size_t label = hash_slice(key) % hmap->size;

  struct AsmSymbolEntry* cur = hmap->arr[label];
  while (cur != NULL){
    if (compare_slice_to_slice(cur->key, key)){
      return true;
    }
    cur = cur->next;
  }
  return false;
}

void print_pseudo_map(struct Slice* func, struct PseudoMap* hmap){
  int args[2];

  asm_gen_fdprint_slice(STDOUT, func);
  fdputs(STDOUT, " pseudo map:\n");
  if (hmap == NULL) {
    fdputs(STDOUT, "  <null>\n");
    return;
  }

  for (size_t i = 0; i < hmap->size; i++){
    struct PseudoEntry* cur = hmap->arr[i];
    while (cur != NULL){
      fdputs(STDOUT, "  Key: ");
      asm_gen_fdprint_slice(STDOUT, cur->pseudo->pseudo);
      fdputs(STDOUT, "\n");

      args[0] = cur->mapped->lit_value;
      fdprintf(STDOUT, "    BP Offset: %d\n", args);
      fdputs(STDOUT, "    Type: ");
      switch (cur->mapped->asm_type->type) {
        case BYTE:
          fdputs(STDOUT, "BYTE\n");
          break;
        case DOUBLE:
          fdputs(STDOUT, "DOUBLE\n");
          break;
        case WORD:
          fdputs(STDOUT, "WORD\n");
          break;
        case LONG_WORD:
          fdputs(STDOUT, "LONG_WORD\n");
          break;
        case BYTE_ARRAY:
          args[0] = (int)cur->mapped->asm_type->byte_array.size;
          args[1] = (int)cur->mapped->asm_type->byte_array.alignment;
          fdprintf(STDOUT, "BYTE_ARRAY(size=%zu, alignment=%zu)\n", args);
          break;
        default:
          fdputs(STDOUT, "unknown\n");
          break;
      }
      cur = cur->next;
    }
  }
}

void print_asm_symbol_table(struct AsmSymbolTable* hmap){
  int args[2];

  if (hmap == NULL) {
    fdputs(STDOUT, "<null> asm symbol table\n");
    return;
  }

  for (size_t i = 0; i < hmap->size; i++){
    struct AsmSymbolEntry* cur = hmap->arr[i];
    while (cur != NULL){
      fdputs(STDOUT, "Key: ");
      asm_gen_fdprint_slice(STDOUT, cur->key);
      fdputs(STDOUT, "\n");
      fdputs(STDOUT, "  Type: ");
      if (cur->type == NULL) {
        fdputs(STDOUT, "NULL\n");
      } else {
        switch (cur->type->type){
          case BYTE:
            fdputs(STDOUT, "BYTE\n");
            break;
          case DOUBLE:
            fdputs(STDOUT, "DOUBLE\n");
            break;
          case WORD:
            fdputs(STDOUT, "WORD\n");
            break;
          case LONG_WORD:
            fdputs(STDOUT, "LONG_WORD\n");
            break;
          case BYTE_ARRAY:
            args[0] = (int)cur->type->byte_array.size;
            args[1] = (int)cur->type->byte_array.alignment;
            fdprintf(STDOUT, "BYTE_ARRAY(size=%zu, alignment=%zu)\n", args);
            break;
          default:
            args[0] = (int)cur->type->type;
            fdprintf(STDOUT, "Unknown (%d)\n", args);
            break;
        }
      }
      args[0] = (int)(cur->is_static ? "true" : "false");
      fdprintf(STDOUT, "  Is Static: %s\n", args);
      args[0] = (int)(cur->is_defined ? "true" : "false");
      fdprintf(STDOUT, "  Is Defined: %s\n", args);
      fdputs(STDOUT, "\n");
      cur = cur->next;
    }
  }
}

size_t asm_type_size(struct AsmType* type){
  switch (type->type){
    case BYTE:
      return 1;
    case DOUBLE:
      return 2;
    case WORD:
      return 4;
    case LONG_WORD:
      return 8;
    case BYTE_ARRAY:
      return type->byte_array.size;
    default:
      asm_gen_error1_int("asm-type-size", NULL,
                         "unknown asm type %d", (int)type->type);
      return 0;
  }
}
