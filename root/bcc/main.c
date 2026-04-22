#include "../crt/print.h"
#include "../crt/stdlib.h"
#include "../crt/string.h"
#include "../crt/fcntl.h"
#include "../crt/unistd.h"
#include "../crt/dirent.h"
#include "../crt/sys/wait.h"

#include "preprocessor.h"
#include "token_array.h"
#include "lexer.h"
#include "parser.h"
#include "identifier_resolution.h"
#include "label_resolution.h"
#include "typechecking.h"
#include "TAC.h"
#include "asm_gen.h"
#include "codegen.h"
#include "machine_print.h"
#include "arena.h"
#include "source_location.h"

#define K_DEFAULT_ASM_OUTPUT "a.s"
#define K_DEFAULT_BIN_OUTPUT "a.bin"
#define K_MAX_FILE_IO_BYTES_PER_SYSCALL 1024
#define K_BCC_PATH "/sbin/bcc"
#define K_BASM_PATH "/sbin/basm"
#define K_CRT_DIR "/crt"
#define K_CRT_BUILD_DIR "/crt/build"

/*
 * The no-`-s` driver path links three buckets in Makefile order:
 * 1. handwritten CRT assembly from `/crt` with `crt0.s` first
 * 2. precompiled CRT helper assembly from `/crt/build`
 * 3. the user translation unit produced by `bcc -s`
 */
static char* kCrtStartupPath = "/crt/crt0.s";

/*
 * Dioptase userland does not yet expose unlink(), so the full-driver path
 * reuses deterministic sidecar assembly file names instead of creating
 * throwaway temporaries it cannot delete later.
 */
static void print_usage(char* program_name) {
  int args[1];

  if (program_name == NULL) {
    fdputs(STDOUT, "usage: bcc [-s] [-g] [-o <file>] [-DNAME[=value]] <file>\n");
    return;
  }

  args[0] = (int)program_name;
  fdprintf(STDOUT, "usage: %s [-s] [-g] [-o <file>] [-DNAME[=value]] <file>\n", args);
}

static void print_message(char* message) {
  int args[1];

  args[0] = (int)message;
  fdprintf(STDOUT, "bcc: %s\n", args);
}

static void print_path_error(char* message, char* path) {
  int args[2];

  args[0] = (int)message;
  args[1] = (int)path;
  fdprintf(STDOUT, "bcc: %s: %s\n", args);
}

static void free_string_array(char** strings, int count) {
  int i;

  if (strings == NULL) {
    return;
  }

  for (i = 0; i < count; ++i) {
    if (strings[i] != NULL) {
      free(strings[i]);
    }
  }

  free(strings);
}

static char* make_temp_asm_path(char* output_path, char* tag) {
  unsigned output_len;
  unsigned tag_len;
  unsigned total_len;
  char* temp_path;

  output_len = strlen(output_path);
  tag_len = strlen(tag);
  total_len = output_len + tag_len + 9;
  temp_path = malloc(total_len);
  if (temp_path == NULL) {
    print_path_error("failed to allocate temp assembly path", output_path);
    return NULL;
  }

  memcpy(temp_path, output_path, output_len);
  memcpy(temp_path + output_len, ".bcc.", 5);
  memcpy(temp_path + output_len + 5, tag, tag_len);
  memcpy(temp_path + output_len + 5 + tag_len, ".s", 3);
  return temp_path;
}

static char* make_define_arg(char* def) {
  unsigned def_len;
  char* arg;

  def_len = strlen(def);
  arg = malloc(def_len + 3);
  if (arg == NULL) {
    print_message("failed to allocate -D forwarding argument");
    return NULL;
  }

  arg[0] = '-';
  arg[1] = 'D';
  memcpy(arg + 2, def, def_len + 1);
  return arg;
}

static bool has_asm_suffix(char* path) {
  unsigned len;

  len = strlen(path);
  if (len < 2) {
    return false;
  }

  return path[len - 2] == '.' && path[len - 1] == 's';
}

static bool has_generated_asm_suffix(char* path) {
  unsigned len;

  len = strlen(path);
  if (len < 6) {
    return false;
  }

  return path[len - 6] == '.' && path[len - 5] == 'g'
    && path[len - 4] == 'e' && path[len - 3] == 'n'
    && path[len - 2] == '.' && path[len - 1] == 's';
}

static bool should_swap_asm_paths(char* left, char* right, char* startup_path) {
  if (startup_path != NULL && strcmp(left, startup_path) == 0) {
    return false;
  }
  if (startup_path != NULL && strcmp(right, startup_path) == 0) {
    return true;
  }
  return strcmp(left, right) > 0;
}

static void sort_asm_paths(char** paths, int count, char* startup_path) {
  int i;
  int j;

  for (i = 0; i < count; ++i) {
    for (j = i + 1; j < count; ++j) {
      if (should_swap_asm_paths(paths[i], paths[j], startup_path)) {
        char* tmp = paths[i];
        paths[i] = paths[j];
        paths[j] = tmp;
      }
    }
  }
}

static char** load_asm_paths_from_dir(char* dir_path, char* startup_path,
                                      bool generated_only, int* out_count) {
  int dir_fd;
  char buffer[K_MAX_FILE_IO_BYTES_PER_SYSCALL];
  char** paths;
  int count;
  int cap;
  bool saw_startup;

  dir_fd = open(dir_path);
  if (dir_fd < 0) {
    print_path_error("failed to open assembly directory", dir_path);
    return NULL;
  }

  paths = NULL;
  count = 0;
  cap = 0;
  saw_startup = false;
  while (true) {
    int bytes_read;
    unsigned offset;

    bytes_read = getdents(dir_fd, buffer, K_MAX_FILE_IO_BYTES_PER_SYSCALL);
    if (bytes_read < 0) {
      print_path_error("failed to read assembly directory", dir_path);
      close(dir_fd);
      free_string_array(paths, count);
      return NULL;
    }
    if (bytes_read == 0) {
      break;
    }

    offset = 0;
    while (offset < (unsigned)bytes_read) {
      struct linux_dirent* entry;
      char* name;

      entry = (struct linux_dirent*)(buffer + offset);
      name = &entry->d_name;
      if ((!generated_only && has_asm_suffix(name))
          || (generated_only && has_generated_asm_suffix(name))) {
        char** grown_paths;

        if (count == cap) {
          cap = cap == 0 ? 8 : cap * 2;
          grown_paths = realloc(paths, sizeof(char*) * (unsigned)cap);
          if (grown_paths == NULL) {
            print_message("failed to grow assembly file list");
            close(dir_fd);
            free_string_array(paths, count);
            return NULL;
          }
          paths = grown_paths;
        }

        paths[count] = malloc(strlen(dir_path) + strlen(name) + 2);
        if (paths[count] == NULL) {
          print_path_error("failed to allocate assembly path", name);
          close(dir_fd);
          free_string_array(paths, count);
          return NULL;
        }
        memcpy(paths[count], dir_path, strlen(dir_path));
        paths[count][strlen(dir_path)] = '/';
        memcpy(paths[count] + strlen(dir_path) + 1, name, strlen(name) + 1);
        if (startup_path != NULL && strcmp(paths[count], startup_path) == 0) {
          saw_startup = true;
        }
        ++count;
      }

      offset += entry->d_reclen;
    }
  }

  if (close(dir_fd) < 0) {
    print_path_error("failed to close assembly directory", dir_path);
    free_string_array(paths, count);
    return NULL;
  }

  if (count == 0) {
    print_path_error("no assembly files were found", dir_path);
    free(paths);
    return NULL;
  }
  if (startup_path != NULL && !saw_startup) {
    print_path_error("missing required CRT startup file", startup_path);
    free_string_array(paths, count);
    return NULL;
  }

  sort_asm_paths(paths, count, startup_path);
  *out_count = count;
  return paths;
}

static char** load_linker_crt_asm_paths(int* out_count) {
  char** crt_paths;
  char** build_paths;
  char** merged_paths;
  int crt_count;
  int build_count;
  int i;

  crt_paths = load_asm_paths_from_dir(K_CRT_DIR, kCrtStartupPath, false,
                                      &crt_count);
  if (crt_paths == NULL) {
    return NULL;
  }

  build_paths = load_asm_paths_from_dir(K_CRT_BUILD_DIR, NULL, true,
                                        &build_count);
  if (build_paths == NULL) {
    free_string_array(crt_paths, crt_count);
    return NULL;
  }

  merged_paths = malloc(sizeof(char*) * (unsigned)(crt_count + build_count));
  if (merged_paths == NULL) {
    print_message("failed to allocate merged CRT assembly list");
    free_string_array(crt_paths, crt_count);
    free_string_array(build_paths, build_count);
    return NULL;
  }

  for (i = 0; i < crt_count; ++i) {
    merged_paths[i] = crt_paths[i];
  }
  for (i = 0; i < build_count; ++i) {
    merged_paths[crt_count + i] = build_paths[i];
  }

  free(crt_paths);
  free(build_paths);
  *out_count = crt_count + build_count;
  return merged_paths;
}

/*
 * Purpose: Spawn `/sbin/bcc -s ...` in a child process so each translation unit
 * gets a fresh compiler instance. This avoids reusing global compiler state
 * before the driver hands the resulting assembly to `basm`.
 */
static bool compile_source_with_child_bcc(char* source_path, char* asm_path,
                                          bool emit_debug_info,
                                          int num_defines, char** cli_defines) {
  char** args;
  int arg_count;
  int define_arg_start;
  int i;
  int pid;
  int status;

  args = malloc(sizeof(char*) * (unsigned)(num_defines + 8));
  if (args == NULL) {
    print_message("failed to allocate compiler child arguments");
    return false;
  }

  arg_count = 0;
  args[arg_count++] = K_BCC_PATH;
  args[arg_count++] = "-s";
  if (emit_debug_info) {
    args[arg_count++] = "-g";
  }
  args[arg_count++] = "-o";
  args[arg_count++] = asm_path;
  define_arg_start = arg_count;
  for (i = 0; i < num_defines; ++i) {
    args[arg_count] = make_define_arg(cli_defines[i]);
    if (args[arg_count] == NULL) {
      for (i = define_arg_start; i < arg_count; ++i) {
        free(args[i]);
      }
      free(args);
      return false;
    }
    ++arg_count;
  }
  args[arg_count++] = source_path;
  args[arg_count] = NULL;

  pid = fork();
  if (pid < 0) {
    print_path_error("failed to fork compiler child", source_path);
    for (i = define_arg_start; i < define_arg_start + num_defines; ++i) {
      free(args[i]);
    }
    free(args);
    return false;
  }

  if (pid == 0) {
    if (execv(K_BCC_PATH, arg_count, args) != 0) {
      print_path_error("failed to exec compiler child", K_BCC_PATH);
      exit(127);
    }
  }

  status = wait_child(pid);
  for (i = define_arg_start; i < define_arg_start + num_defines; ++i) {
    free(args[i]);
  }
  free(args);
  if (status < 0) {
    print_path_error("failed to wait for compiler child", source_path);
    return false;
  }
  if (status != 0) {
    print_path_error("compiler child failed", source_path);
    return false;
  }

  return true;
}

static bool exec_basm_for_binary(char* output_path, char* user_asm_path,
                                 char** crt_asm_paths, int crt_asm_count) {
  char** args;
  int arg_count;
  int i;

  arg_count = 4;
  arg_count += crt_asm_count;
  arg_count += 1;

  args = malloc(sizeof(char*) * (unsigned)(arg_count + 1));
  if (args == NULL) {
    print_message("failed to allocate assembler arguments");
    return false;
  }

  arg_count = 0;
  args[arg_count++] = K_BASM_PATH;
  args[arg_count++] = "-bin";
  args[arg_count++] = "-o";
  args[arg_count++] = output_path;
  for (i = 0; i < crt_asm_count; ++i) {
    args[arg_count++] = crt_asm_paths[i];
  }
  args[arg_count++] = user_asm_path;
  args[arg_count] = NULL;

  if (execv(K_BASM_PATH, arg_count, args) != 0) {
    print_path_error("failed to exec assembler", K_BASM_PATH);
    free(args);
    return false;
  }

  return true;
}

static bool compile_and_exec_binary(char* source_path, char* output_path,
                                    bool emit_debug_info,
                                    int num_defines, char** cli_defines) {
  char* user_asm_path;
  char** crt_asm_paths;
  int crt_asm_count;

  user_asm_path = make_temp_asm_path(output_path, "input");
  if (user_asm_path == NULL) {
    return false;
  }

  if (!compile_source_with_child_bcc(source_path, user_asm_path, emit_debug_info,
                                     num_defines, cli_defines)) {
    free(user_asm_path);
    return false;
  }

  crt_asm_paths = load_linker_crt_asm_paths(&crt_asm_count);
  if (crt_asm_paths == NULL) {
    free(user_asm_path);
    return false;
  }

  if (!exec_basm_for_binary(output_path, user_asm_path, crt_asm_paths,
                            crt_asm_count)) {
    free_string_array(crt_asm_paths, crt_asm_count);
    free(user_asm_path);
    return false;
  }

  return true;
}

static char* load_source_file(char* file_path) {
  int fd;
  int file_size;
  unsigned copied;
  char* bytes;

  fd = open(file_path);
  if (fd < 0) {
    print_path_error("failed to open source file", file_path);
    return NULL;
  }

  file_size = seek(fd, 0, SEEK_END);
  if (file_size < 0) {
    print_path_error("failed to measure source file", file_path);
    close(fd);
    return NULL;
  }

  if (seek(fd, 0, SEEK_SET) < 0) {
    print_path_error("failed to rewind source file", file_path);
    close(fd);
    return NULL;
  }

  bytes = malloc((unsigned)file_size + 1);
  if (bytes == NULL) {
    print_path_error("failed to allocate source buffer", file_path);
    close(fd);
    return NULL;
  }

  copied = 0;
  while (copied < (unsigned)file_size) {
    unsigned chunk_size;
    int read_count;

    chunk_size = (unsigned)file_size - copied;
    if (chunk_size > K_MAX_FILE_IO_BYTES_PER_SYSCALL) {
      chunk_size = K_MAX_FILE_IO_BYTES_PER_SYSCALL;
    }

    read_count = read(fd, bytes + copied, chunk_size);
    if (read_count <= 0) {
      print_path_error("failed to read source file", file_path);
      free(bytes);
      close(fd);
      return NULL;
    }

    copied += (unsigned)read_count;
  }

  bytes[file_size] = '\0';

  if (close(fd) < 0) {
    print_path_error("failed to close source file", file_path);
    free(bytes);
    return NULL;
  }

  return bytes;
}

int main(int argc, char** argv) {
  char* filename;
  char* output_path;
  char** cli_defines;
  int num_defines;
  bool emit_debug_info;
  bool saw_emit_asm_flag;
  int i;
  char* source_text;
  struct PreprocessResult preprocessed;
  struct TokenArray* tokens;
  struct Program* prog;
  struct TACProg* tac_prog;
  struct AsmProg* asm_prog;
  struct MachineProg* machine_prog;

  filename = NULL;
  output_path = NULL;
  cli_defines = NULL;
  num_defines = 0;
  emit_debug_info = false;
  saw_emit_asm_flag = false;
  source_text = NULL;
  preprocessed.text = NULL;
  preprocessed.map.entries = NULL;
  preprocessed.map.length = 0;
  preprocessed.file_table.names = NULL;
  preprocessed.file_table.count = 0;
  preprocessed.file_table.cap = 0;
  tokens = NULL;
  prog = NULL;
  tac_prog = NULL;
  asm_prog = NULL;
  machine_prog = NULL;

  if (argc <= 0) {
    print_usage(NULL);
    return 1;
  }

  cli_defines = malloc(sizeof(char*) * (unsigned)argc);
  if (cli_defines == NULL) {
    print_message("failed to allocate argument bookkeeping");
    return 1;
  }

  for (i = 1; i < argc; ++i) {
    char* arg = argv[i];

    if (strcmp(arg, "-s") == 0) {
      saw_emit_asm_flag = true;
      continue;
    }
    if (strcmp(arg, "-g") == 0) {
      emit_debug_info = true;
      continue;
    }
    if (strcmp(arg, "-o") == 0) {
      if (i + 1 >= argc) {
        print_message("option -o requires an output file path");
        free(cli_defines);
        return 1;
      }
      output_path = argv[++i];
      continue;
    }
    if (strncmp(arg, "-D", 2) == 0) {
      char* def = arg + 2;
      if (def[0] == '\0') {
        print_message("invalid -D definition");
        free(cli_defines);
        return 1;
      }
      cli_defines[num_defines++] = def;
      continue;
    }
    if (arg[0] == '-') {
      print_message("this bootstrap bcc build only supports -s, -g, -o, and -D");
      print_usage(argv[0]);
      free(cli_defines);
      return 1;
    }
    if (filename == NULL) {
      filename = arg;
      continue;
    }

    print_usage(argv[0]);
    free(cli_defines);
    return 1;
  }

  if (filename == NULL) {
    print_usage(argv[0]);
    free(cli_defines);
    return 1;
  }

  if (output_path == NULL) {
    if (saw_emit_asm_flag) {
      output_path = K_DEFAULT_ASM_OUTPUT;
    } else {
      output_path = K_DEFAULT_BIN_OUTPUT;
    }
  }

  if (!saw_emit_asm_flag) {
    if (!compile_and_exec_binary(filename, output_path, emit_debug_info,
                                 num_defines, cli_defines)) {
      free(cli_defines);
      return 1;
    }
    free(cli_defines);
    return 0;
  }

  source_text = load_source_file(filename);
  if (source_text == NULL) {
    free(cli_defines);
    return 1;
  }

  puts("Preprocessing...\n");
  if (!preprocess(source_text, filename, num_defines, cli_defines, &preprocessed)) {
    free(source_text);
    free(cli_defines);
    return 1;
  }
  free(cli_defines);
  cli_defines = NULL;
  free(source_text);
  source_text = NULL;

  set_source_context_with_map(filename, preprocessed.text, &preprocessed.map);

  puts("Lexing...\n");
  tokens = lex(preprocessed.text);
  if (tokens == NULL) {
    destroy_preprocess_result(&preprocessed);
    return 1;
  }

  arena_init(16384);

  puts("Parsing...\n");
  prog = parse_prog(tokens);
  if (prog == NULL) {
    destroy_token_array(tokens);
    destroy_preprocess_result(&preprocessed);
    arena_destroy();
    return 1;
  }

  puts("Resolving identifiers...\n");
  if (!resolve_prog(prog)) {
    print_message("identifier resolution failed");
    destroy_token_array(tokens);
    destroy_preprocess_result(&preprocessed);
    arena_destroy();
    return 1;
  }

  puts("Resolving labels...\n");
  if (!label_loops(prog)) {
    print_message("label resolution failed");
    destroy_token_array(tokens);
    destroy_preprocess_result(&preprocessed);
    arena_destroy();
    return 1;
  }

  puts("Typechecking...\n");
  if (!typecheck_program(prog)) {
    print_message("typechecking failed");
    destroy_token_array(tokens);
    destroy_preprocess_result(&preprocessed);
    arena_destroy();
    return 1;
  }

  puts("Lowering to TAC...\n");
  tac_prog = prog_to_TAC(prog, emit_debug_info);
  if (tac_prog == NULL) {
    print_message("TAC lowering failed");
    destroy_token_array(tokens);
    destroy_preprocess_result(&preprocessed);
    arena_destroy();
    return 1;
  }

  puts("Lowering to assembly...\n");
  asm_prog = prog_to_asm(tac_prog, true);
  if (asm_prog == NULL) {
    print_message("assembly lowering failed");
    destroy_token_array(tokens);
    destroy_preprocess_result(&preprocessed);
    arena_destroy();
    return 1;
  }

  machine_prog = prog_to_machine(asm_prog);
  if (machine_prog == NULL) {
    print_message("machine lowering failed");
    destroy_token_array(tokens);
    destroy_preprocess_result(&preprocessed);
    arena_destroy();
    return 1;
  }

  puts("Writing assembly output...\n");
  if (!write_machine_prog_to_file(machine_prog, output_path)) {
    print_path_error("failed to write assembly output", output_path);
    destroy_token_array(tokens);
    destroy_preprocess_result(&preprocessed);
    arena_destroy();
    return 1;
  }

  destroy_token_array(tokens);
  destroy_preprocess_result(&preprocessed);
  arena_destroy();
  return 0;
}
