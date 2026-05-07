#include "../crt/print.h"
#include "../crt/stdlib.h"
#include "../crt/string.h"

#include "slice.h"
#include "preprocessor.h"
#include "assembler.h"

static unsigned result_index;
static char* result;
static unsigned capacity;

#define K_INITIAL_RESULT_CAPACITY 60
#define K_MAX_UNSIGNED_DECIMAL_DIGITS 10
#define K_MAX_UNSIGNED_HEX_DIGITS 8

static void print_preprocessor_memory_error(void) {
  puts("Preprocessor memory error\n");
}

static bool expand_capacity(unsigned minimum_extra_bytes) {
  unsigned new_capacity;
  char* new_result;

  new_capacity = capacity * 2;
  while (result_index + minimum_extra_bytes >= new_capacity - 1) {
    new_capacity *= 2;
  }

  new_result = malloc(new_capacity);
  if (new_result == NULL) {
    print_preprocessor_memory_error();
    return false;
  }

  memcpy(new_result, result, result_index);
  free(result);
  result = new_result;
  capacity = new_capacity;
  return true;
}

static bool ensure_capacity(unsigned minimum_extra_bytes) {
  if (result_index + minimum_extra_bytes < capacity - 1) return true;
  return expand_capacity(minimum_extra_bytes);
}

static bool append_char(char c) {
  if (!ensure_capacity(2)) return false;
  result[result_index] = c;
  result_index += 1;
  return true;
}

static bool append_cstr(char* str) {
  unsigned len;
  if (str == NULL) return false;
  len = strlen(str);
  if (!ensure_capacity(len + 1)) return false;
  memcpy(result + result_index, str, len);
  result_index += len;
  return true;
}

static bool append_slice(struct Slice* slice) {
  if (slice == NULL) return false;
  if (!ensure_capacity(slice->len + 1)) return false;
  memcpy(result + result_index, slice->start, slice->len);
  result_index += slice->len;
  return true;
}

static bool append_unsigned_decimal(unsigned value) {
  char digits[K_MAX_UNSIGNED_DECIMAL_DIGITS];
  unsigned digit_count;

  digit_count = 0;
  do {
    digits[digit_count] = (char)('0' + (value % 10));
    value /= 10;
    digit_count += 1;
  } while (value != 0);

  if (!ensure_capacity(digit_count + 1)) return false;

  while (digit_count > 0) {
    digit_count -= 1;
    result[result_index] = digits[digit_count];
    result_index += 1;
  }

  return true;
}

static bool append_unsigned_hex(unsigned value) {
  char digits[K_MAX_UNSIGNED_HEX_DIGITS];
  unsigned digit_count;
  char nibble;

  digit_count = 0;
  do {
    nibble = (char)(value & 0xF);
    if (nibble < 10) {
      digits[digit_count] = (char)('0' + nibble);
    } else {
      digits[digit_count] = (char)('A' + (nibble - 10));
    }
    value >>= 4;
    digit_count += 1;
  } while (value != 0);

  if (!ensure_capacity(digit_count + 1)) return false;

  while (digit_count > 0) {
    digit_count -= 1;
    result[result_index] = digits[digit_count];
    result_index += 1;
  }

  return true;
}

static bool append_register_name(int reg) {
  if (!append_char('r')) return false;
  return append_unsigned_decimal((unsigned)reg);
}

static bool append_control_register_name(int reg) {
  if (!append_cstr("cr")) return false;
  return append_unsigned_decimal((unsigned)reg);
}

static void print_invalid_register_error(void) {
  print_error();
  puts("Invalid register\n");
  puts("Valid registers are r0 - r31\n");
}

static void print_expected_immediate_error(void) {
  print_error();
  puts("Expected immediate\n");
}

static bool check_capacity(void) {
  return ensure_capacity(2);
}

static bool skip_comments(void) {
  if (*current == '#') {
    while (*current != '\n') {
      if (*current == '\0') return false;
      current += 1;
    }
  }
  return true;
}

static bool expand_nop(void) {
  return append_cstr("and  r0, r0, r0");
}

static bool expand_ret(void) {
  return append_cstr("jmp  r29");
}

static void expand_push(bool* success) {
  int ra;

  ra = consume_register();
  if (ra == -1) {
    print_invalid_register_error();
    *success = false;
    return;
  }

  *success =
    append_cstr("swa  ") &&
    append_register_name(ra) &&
    append_cstr(" [sp, -4]!");
}

static void expand_pop(bool* success) {
  int ra;

  ra = consume_register();
  if (ra == -1) {
    print_invalid_register_error();
    *success = false;
    return;
  }

  *success =
    append_cstr("lwa  ") &&
    append_register_name(ra) &&
    append_cstr(", [sp], 4");
}

static void expand_pshd(bool* success) {
  int ra;

  ra = consume_register();
  if (ra == -1) {
    print_invalid_register_error();
    *success = false;
    return;
  }

  *success =
    append_cstr("sda  ") &&
    append_register_name(ra) &&
    append_cstr(" [sp, -2]!");
}

static void expand_popd(bool* success) {
  int ra;

  ra = consume_register();
  if (ra == -1) {
    print_invalid_register_error();
    *success = false;
    return;
  }

  *success =
    append_cstr("lda  ") &&
    append_register_name(ra) &&
    append_cstr(", [sp], 2");
}

static void expand_pshb(bool* success) {
  int ra;

  ra = consume_register();
  if (ra == -1) {
    print_invalid_register_error();
    *success = false;
    return;
  }

  *success =
    append_cstr("sba  ") &&
    append_register_name(ra) &&
    append_cstr(" [sp, -1]!");
}

static void expand_popb(bool* success) {
  int ra;

  ra = consume_register();
  if (ra == -1) {
    print_invalid_register_error();
    *success = false;
    return;
  }

  *success =
    append_cstr("lba  ") &&
    append_register_name(ra) &&
    append_cstr(", [sp], 1");
}

static void expand_movi(bool* success) {
  int ra;
  enum ConsumeResult c_result;
  int imm;
  struct Slice* label;

  ra = consume_register();
  if (ra == -1) {
    print_invalid_register_error();
    *success = false;
    return;
  }

  imm = consume_literal(&c_result);
  if (c_result == FOUND) {
    *success =
      append_cstr("movu ") &&
      append_register_name(ra) &&
      append_cstr(", 0x") &&
      append_unsigned_hex((unsigned)imm) &&
      append_cstr("; movl ") &&
      append_register_name(ra) &&
      append_cstr(", 0x") &&
      append_unsigned_hex((unsigned)imm);
    return;
  }

  label = consume_identifier();
  if (label == NULL) {
    print_expected_immediate_error();
    *success = false;
    return;
  }

  *success =
    append_cstr("movu ") &&
    append_register_name(ra) &&
    append_cstr(", ") &&
    append_slice(label) &&
    append_cstr("; movl ") &&
    append_register_name(ra) &&
    append_cstr(", ") &&
    append_slice(label);

  free(label);
}

static void expand_mov(bool* success) {
  int ra;
  int rb;

  ra = consume_register();
  if (ra == -1) {
    ra = consume_control_register();
    if (ra == -1) {
      print_invalid_register_error();
      *success = false;
      return;
    }

    rb = consume_register();
    if (rb == -1) {
      rb = consume_control_register();
      if (rb == -1) {
        print_invalid_register_error();
        *success = false;
        return;
      }

      *success =
        append_cstr("crmv ") &&
        append_control_register_name(ra) &&
        append_cstr(", ") &&
        append_control_register_name(rb);
      return;
    }

    *success =
      append_cstr("crmv ") &&
      append_control_register_name(ra) &&
      append_cstr(", ") &&
      append_register_name(rb);
    return;
  }

  rb = consume_register();
  if (rb == -1) {
    rb = consume_control_register();
    if (rb == -1) {
      print_invalid_register_error();
      *success = false;
      return;
    }

    *success =
      append_cstr("crmv ") &&
      append_register_name(ra) &&
      append_cstr(", ") &&
      append_control_register_name(rb);
    return;
  }

  *success =
    append_cstr("add  ") &&
    append_register_name(ra) &&
    append_cstr(", ") &&
    append_register_name(rb) &&
    append_cstr(", r0");
}

static void expand_call(bool* success) {
  enum ConsumeResult c_result;
  int imm;
  struct Slice* label;

  imm = consume_literal(&c_result);
  if (c_result == FOUND) {
    *success =
      append_cstr("movu r29, 0x") &&
      append_unsigned_hex((unsigned)imm) &&
      append_cstr("; movl r29, 0x") &&
      append_unsigned_hex((unsigned)imm) &&
      append_cstr("; br r29, r29");
    return;
  }

  label = consume_identifier();
  if (label == NULL) {
    print_expected_immediate_error();
    *success = false;
    return;
  }

  *success =
    append_cstr("movu r29, ") &&
    append_slice(label) &&
    append_cstr("; movl r29, ") &&
    append_slice(label) &&
    append_cstr("; br r29, r29");

  free(label);
}

static bool expand_macros(void) {
  bool success;

  success = true;
  if (consume_keyword("nop")) success = expand_nop();
  else if (consume_keyword("ret")) success = expand_ret();
  else if (consume_keyword("push")) expand_push(&success);
  else if (consume_keyword("pop")) expand_pop(&success);
  else if (consume_keyword("pshw")) expand_push(&success);
  else if (consume_keyword("popw")) expand_pop(&success);
  else if (consume_keyword("pshd")) expand_pshd(&success);
  else if (consume_keyword("popd")) expand_popd(&success);
  else if (consume_keyword("pshb")) expand_pshb(&success);
  else if (consume_keyword("popb")) expand_popb(&success);
  else if (consume_keyword("movi")) expand_movi(&success);
  else if (consume_keyword("mov")) expand_mov(&success);
  else if (consume_keyword("call")) expand_call(&success);

  if (!success) puts("Preprocessor macro error\n");
  return success;
}

char** preprocess(int num_files, int* file_names, bool is_kernel, char** argv, char** files) {
  char** result_list;

  result_list = malloc(num_files * sizeof(char*));
  if (result_list == NULL) {
    print_preprocessor_memory_error();
    return NULL;
  }

  for (int i = 0; i < num_files; ++i) {
    current = files[i];
    current_buffer_start = current;
    line_count = 1;
    pc = is_kernel ? 0 : 0x80000000;
    result_index = 0;
    capacity = K_INITIAL_RESULT_CAPACITY;
    current_file = argv[file_names[i]];

    result = malloc(capacity);
    if (result == NULL) {
      print_preprocessor_memory_error();
      for (int j = 0; j < i; ++j) free(result_list[j]);
      free(result_list);
      return NULL;
    }

    result[result_index] = '\0';
    result_index += 1;

    while (*current != '\0') {
      if (!check_capacity()) {
        for (int j = 0; j < i; ++j) free(result_list[j]);
        free(result);
        free(result_list);
        return NULL;
      }

      if (!skip_comments()) goto done_with_file;

      if (!expand_macros()) {
        for (int j = 0; j < i; ++j) free(result_list[j]);
        free(result);
        free(result_list);
        return NULL;
      }

      result[result_index] = *current;
      if (*current == '\n') line_count += 1;
      result_index += 1;
      current += 1;
    }

    done_with_file:
    result[result_index] = '\0';
    result_list[i] = result;
  }

  return result_list;
}
