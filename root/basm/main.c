#include "../crt/print.h"
#include "../crt/stdlib.h"
#include "../crt/string.h"
#include "../crt/fcntl.h"
#include "../crt/unistd.h"

#include "assembler.h"
#include "instruction_array.h"
#include "label_list.h"
#include "preprocessor.h"
#include "elf.h"
#include "debug.h"

// Purpose: User/kernel read and write traps currently clamp each transfer to
// 1024 bytes, so large source files must be copied in chunks.
#define K_MAX_FILE_IO_BYTES_PER_SYSCALL 1024

static void print_usage(char* program_name) {
  void* args[1];
  if (program_name == NULL) {
    fdputs(STDOUT, "usage: basm <file name>\n");
    return;
  }
  args[0] = program_name;
  fdprintf(STDOUT, "usage: %s <file name>\n", args);
}

static void print_basm_message(char* message) {
  void* args[1];
  args[0] = message;
  fdprintf(STDOUT, "basm: %s\n", args);
}

static void print_basm_path_error(char* message, char* path) {
  void* args[2];
  args[0] = message;
  args[1] = path;
  fdprintf(STDOUT, "basm: %s: %s\n", args);
}

// Purpose: Join two path components with a '/' separator when needed.
// Inputs: left and right are path components.
// Outputs: Returns a heap-allocated joined path or NULL on allocation failure.
// Invariants/Assumptions: Paths in Dioptase-OS use '/' as the separator.
static char* join_paths(char* left, char* right) {
  unsigned left_len;
  unsigned right_len;
  unsigned total_len;
  unsigned index;
  bool needs_sep;
  char* path;

  if (left == NULL || right == NULL) return NULL;

  left_len = strlen(left);
  right_len = strlen(right);
  needs_sep = (left_len > 0 && left[left_len - 1] != '/');
  total_len = left_len + (needs_sep ? 1 : 0) + right_len + 1;
  path = malloc(total_len);
  if (path == NULL) return NULL;

  memcpy(path, left, left_len);
  index = left_len;
  if (needs_sep) {
    path[index] = '/';
    index += 1;
  }
  memcpy(path + index, right, right_len);
  index += right_len;
  path[index] = '\0';
  return path;
}

static void free_loaded_files(char** files, int count) {
  if (files == NULL) return;
  for (int i = 0; i < count; ++i) {
    if (files[i] != NULL) free(files[i]);
  }
  free(files);
}

// Purpose: Read the full source file into user-owned heap memory.
// Inputs: file_path names the source file to copy.
// Outputs: Returns a NUL-terminated heap buffer on success, NULL on failure.
// Invariants/Assumptions: `open()` may create a missing file, so an empty
// source here can mean either an intentionally empty file or a missing path.
static char* load_source_file(char* file_path) {
  int fd;
  int file_size;
  unsigned copied;
  char* bytes;

  fd = open(file_path);
  if (fd < 0) {
    print_basm_path_error("failed to open source file", file_path);
    return NULL;
  }

  file_size = seek(fd, 0, SEEK_END);
  if (file_size < 0) {
    print_basm_path_error("failed to measure source file", file_path);
    close(fd);
    return NULL;
  }

  if (seek(fd, 0, SEEK_SET) < 0) {
    print_basm_path_error("failed to rewind source file", file_path);
    close(fd);
    return NULL;
  }

  bytes = malloc((unsigned)file_size + 1);
  if (bytes == NULL) {
    print_basm_path_error("failed to allocate source buffer", file_path);
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
      print_basm_path_error("failed to read source file", file_path);
      free(bytes);
      close(fd);
      return NULL;
    }

    copied += (unsigned)read_count;
  }

  bytes[file_size] = '\0';

  if (close(fd) < 0) {
    print_basm_path_error("failed to close source file", file_path);
    free(bytes);
    return NULL;
  }

  return bytes;
}

// Purpose: Open the output file as a fresh byte stream for assembly output.
// Inputs: target_name names the destination file.
// Outputs: Returns a writable fd positioned at offset 0, or -1 on failure.
// Invariants/Assumptions: The syscall interface exposes `open()` plus
// shrink-only `truncate()`, so callers must explicitly clear old contents.
static int open_output_file(char* target_name) {
  int fd;

  fd = open(target_name);
  if (fd < 0) {
    print_basm_path_error("failed to open output file", target_name);
    return -1;
  }

  if (truncate(fd, 0) < 0) {
    print_basm_path_error("failed to truncate output file", target_name);
    close(fd);
    return -1;
  }

  if (seek(fd, 0, SEEK_SET) < 0) {
    print_basm_path_error("failed to rewind output file", target_name);
    close(fd);
    return -1;
  }

  return fd;
}

int main(int argc, char** argv) {
  int* file_names;
  int num_files;
  char* target_name;
  bool target_name_default;
  bool pre_only;
  bool is_kernel;
  bool debug_labels;
  bool output_binary;
  char** cli_defines;
  int num_defines;
  char** input_args;
  char** input_args_alloc;
  char** files;
  char** preprocessed;
  struct LabelList* labels;
  struct DebugInfoList* labels_c;
  struct ProgramDescriptor* program;
  int output_fd;

  if (argc <= 0) {
    print_usage(NULL);
    return 1;
  }

  file_names = malloc(argc * sizeof(int));
  cli_defines = malloc(argc * sizeof(char*));
  if (file_names == NULL || cli_defines == NULL) {
    print_basm_message("failed to allocate argument bookkeeping");
    if (file_names != NULL) free(file_names);
    if (cli_defines != NULL) free(cli_defines);
    return 1;
  }

  num_files = 0;
  target_name = "./a.hex";
  target_name_default = true;
  pre_only = false;
  is_kernel = false;
  debug_labels = false;
  output_binary = false;
  num_defines = 0;
  input_args = argv;
  input_args_alloc = NULL;
  files = NULL;
  preprocessed = NULL;
  labels = NULL;
  labels_c = NULL;
  program = NULL;
  output_fd = -1;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-pre") == 0) {
      pre_only = true;
    } else if (strcmp(argv[i], "-o") == 0) {
      if (i + 1 == argc) {
        print_basm_message("must specify a target name after -o");
        free(file_names);
        free(cli_defines);
        return 1;
      }
      target_name = argv[i + 1];
      target_name_default = false;
      ++i;
    } else if (strcmp(argv[i], "-bin") == 0) {
      output_binary = true;
    } else if (strcmp(argv[i], "-kernel") == 0) {
      is_kernel = true;
    } else if (strcmp(argv[i], "-g") == 0) {
      debug_labels = true;
    } else if (strncmp(argv[i], "-D", 2) == 0) {
      char* def;
      def = argv[i] + 2;
      if (def[0] == '\0' || strchr(def, '=') == NULL) {
        print_basm_message("invalid -D definition (expected -DNAME=value)");
        free(file_names);
        free(cli_defines);
        return 1;
      }
      cli_defines[num_defines++] = def;
    } else if (argv[i][0] == '-') {
      void* args[1];
      args[0] = argv[i];
      fdprintf(STDOUT,
        "basm: unrecognized flag %s. Allowed flags are -pre, -o, -bin, -kernel, -g, or -DNAME=value\n",
        args);
      free(file_names);
      free(cli_defines);
      return 1;
    } else {
      file_names[num_files++] = i;
    }
  }

  if (num_files <= 0) {
    print_usage(argv[0]);
    free(file_names);
    free(cli_defines);
    return 1;
  }

  if (output_binary && debug_labels) {
    print_basm_message("-bin output does not support -g debug labels");
    free(file_names);
    free(cli_defines);
    return 1;
  }

  if (output_binary && target_name_default) {
    target_name = "./a.bin";
  }

  files = malloc(num_files * sizeof(char*));
  if (files == NULL) {
    print_basm_message("failed to allocate source file list");
    free(file_names);
    free(cli_defines);
    free(input_args_alloc);
    return 1;
  }
  for (int i = 0; i < num_files; ++i) {
    files[i] = NULL;
  }

  for (int i = 0; i < num_files; ++i) {
    files[i] = load_source_file(input_args[file_names[i]]);
    if (files[i] == NULL) {
      free_loaded_files(files, num_files);
      free(file_names);
      free(cli_defines);
      free(input_args_alloc);
      return 1;
    }
  }

  puts("Preprocessing...\n");
  preprocessed = preprocess(num_files, file_names, is_kernel, input_args, files);
  free_loaded_files(files, num_files);
  files = NULL;
  if (preprocessed == NULL) {
    free(file_names);
    free(cli_defines);
    free(input_args_alloc);
    return 1;
  }

  if (pre_only) {
    for (int i = 0; i < num_files; ++i) {
      fdputs(STDOUT, preprocessed[i] + 1);
      putchar('\n');
    }

    for (int i = 0; i < num_files; ++i) free(preprocessed[i]);
    free(preprocessed);
    free(file_names);
    free(cli_defines);
    free(input_args_alloc);
    return 0;
  }

  set_cli_defines(num_defines, cli_defines);
  program = assemble(
    num_files,
    file_names,
    is_kernel,
    input_args,
    preprocessed,
    debug_labels ? &labels : NULL,
    debug_labels ? &labels_c : NULL
  );

  for (int i = 0; i < num_files; ++i) free(preprocessed[i]);
  free(preprocessed);
  preprocessed = NULL;
  free(file_names);
  file_names = NULL;
  free(cli_defines);
  cli_defines = NULL;
  free(input_args_alloc);
  input_args_alloc = NULL;

  if (program == NULL) {
    if (labels != NULL) destroy_label_list(labels);
    if (labels_c != NULL) destroy_debug_info_list(labels_c);
    return 1;
  }

  output_fd = open_output_file(target_name);
  if (output_fd < 0) {
    destroy_program_descriptor(program);
    if (labels != NULL) destroy_label_list(labels);
    if (labels_c != NULL) destroy_debug_info_list(labels_c);
    return 1;
  }

  if (output_binary) {
    puts("Writing output...\n");
    if (is_kernel) {
      fwrite_instruction_array_list(output_fd, program->sections, true);
    } else {
      struct ElfHeader header;
      struct ElfProgramHeader* pht;

      header = create_elf_header(program);
      fwrite_elf_header(output_fd, &header);

      pht = create_PHT(program);
      fwrite_pht(output_fd, pht);
      free(pht);

      fwrite_instruction_array_list(output_fd, program->sections, false);
    }

    destroy_program_descriptor(program);
  } else {
    if (is_kernel) {
      fprint_instruction_array_list(output_fd, program->sections, true);
    } else {
      struct ElfHeader header;
      struct ElfProgramHeader* pht;

      header = create_elf_header(program);
      fprint_elf_header(output_fd, &header);

      pht = create_PHT(program);
      fprint_pht(output_fd, pht);
      free(pht);

      fprint_instruction_array_list(output_fd, program->sections, false);
    }

    destroy_program_descriptor(program);

    if (debug_labels) {
      if (is_kernel) {
        fprint_label_list_kernel(output_fd, labels);
      } else {
        fprint_label_list(output_fd, labels);
      }
      destroy_label_list(labels);
      labels = NULL;
      fprint_debug_info_list(output_fd, labels_c);
      destroy_debug_info_list(labels_c);
      labels_c = NULL;
    }
  }

  if (close(output_fd) < 0) {
    print_basm_path_error("failed to close output file", target_name);
    return 1;
  }

  return 0;
}
