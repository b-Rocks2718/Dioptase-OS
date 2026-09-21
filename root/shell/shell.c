#include "../crt/print.h"
#include "../crt/sys.h"
#include "../crt/stdbool.h"
#include "../crt/string.h"
#include "../crt/stdlib.h"
#include "../crt/fcntl.h"
#include "../crt/unistd.h"
#include "../crt/sys/wait.h"
#include "../crt/terminal.h"

#include "dirs.h"
#include "shell.h"

#define MAX_ARGV 16
#define FOREGROUND_START_GATE_CLOSED 0
#define SBIN_PREFIX "/sbin/"
// One cwd snapshot plus two normalized paths.
#define PATH_COMPARISON_BUFFER_COUNT 3

char cmd_buf[SHELL_COMMAND_BUFFER_SIZE];
unsigned cmd_buf_len = 0;

// Render the colored machine, working-directory, and prompt prefix.
void print_line_prefix(void){
  // machine name in green
  puts("\x1b[32mdioptase");

  // colon in white
  puts("\x1b[37m:");

  // current directory in blue
  char cwd_buf[MAX_PATH];
  if (getcwd(cwd_buf, MAX_PATH) != (char*)-1){
    puts("\x1b[34m");
    puts(cwd_buf);
  } else {
    puts("\x1b[31m");
    puts("unknown");
  }

  // dollar sign prompt in white
  puts("\x1b[37m$ ");
}

// NUL-terminate and display the current command-line buffer.
void print_cmd_buf(void){
  cmd_buf[cmd_buf_len] = '\0';
  puts(cmd_buf);
}

// Split the current command buffer on spaces into newly allocated arguments.
void parse_command(unsigned* argc_out, char*** argv_out){
  // split command into argv by spaces, ignoring multiple spaces
  char** argv = malloc(sizeof(char*) * MAX_ARGV);
  unsigned argc = 0;

  unsigned i = 0;
  unsigned start = 0;
  while (i < cmd_buf_len && argc < MAX_ARGV){
    // skip leading spaces
    while (i < cmd_buf_len && cmd_buf[i] == ' '){
      i++;
    }

    // everything was spaces
    if (i >= cmd_buf_len){
      break;
    }

    start = i;

    // find next space
    while (i < cmd_buf_len && cmd_buf[i] != ' '){
      i++;
    }

    // copy argument into argv
    argv[argc++] = malloc(i - start + 1);
    memcpy(argv[argc - 1], &cmd_buf[start], i - start);
    argv[argc - 1][i - start] = '\0';
  }

  *argc_out = argc;
  *argv_out = argv;
}

// Free every parsed argument string and its pointer array.
void free_argv(unsigned argc, char** argv){
  for (unsigned i = 0; i < argc; i++){
    free(argv[i]);
  }
  free(argv);
}

// Append path components to an absolute, normalized path. The output buffer
// always contains a NUL-terminated path rooted at `/`, and normalized_length
// never includes that NUL. `..` stops at the root rather than walking above it.
// Every append reserves space for the terminator before writing to normalized.
static bool append_normalized_components(char* path, char* normalized,
                                         unsigned* normalized_length){
  unsigned i = 0;
  while (path[i] != '\0'){
    // Repeated separators have the same lexical meaning as one separator.
    while (path[i] == '/'){
      i++;
    }
    if (path[i] == '\0'){
      break;
    }

    unsigned component_start = i;
    while (path[i] != '\0' && path[i] != '/'){
      i++;
    }
    unsigned component_length = i - component_start;

    if (component_length == 1 && path[component_start] == '.'){
      continue;
    }
    if (component_length == 2 && path[component_start] == '.' &&
        path[component_start + 1] == '.'){
      while (*normalized_length > 1 &&
             normalized[*normalized_length - 1] != '/'){
        (*normalized_length)--;
      }
      if (*normalized_length > 1){
        (*normalized_length)--;
      }
      normalized[*normalized_length] = '\0';
      continue;
    }

    unsigned separator_length = *normalized_length > 1 ? 1 : 0;
    if (*normalized_length >= MAX_PATH){
      return false;
    }
    unsigned available = MAX_PATH - *normalized_length - 1;
    if (separator_length > available){
      return false;
    }
    available -= separator_length;
    if (component_length > available){
      return false;
    }

    if (separator_length != 0){
      normalized[(*normalized_length)++] = '/';
    }
    memcpy(normalized + *normalized_length, path + component_start,
           component_length);
    *normalized_length += component_length;
    normalized[*normalized_length] = '\0';
  }

  return true;
}

// Normalize a path spelling before comparing it with another path.
static bool normalize_path_for_comparison(char* path, char* cwd,
                                          char* normalized){
  unsigned normalized_length = 1;
  normalized[0] = '/';
  normalized[1] = '\0';

  if (path[0] != '/'){
    if (cwd == (char*)0 || cwd[0] != '/'){
      return false;
    }
    if (!append_normalized_components(cwd, normalized,
                                      &normalized_length)){
      return false;
    }
  }

  return append_normalized_components(path, normalized, &normalized_length);
}

// Return 1 for lexical equality, 0 for different paths, and -1 when either
// path cannot be normalized safely. Relative paths use one cwd snapshot so
// both names are interpreted against exactly the same directory.
static int compare_lexical_paths(char* left, char* right){
  /*
   * Three MAX_PATH automatic arrays exceed the ISA's signed 12-bit stack-
   * offset range after the compiler adds this function's other locals. Keep
   * the bounded scratch area in one heap allocation instead: cwd, normalized
   * left, then normalized right.
   */
  char* buffers = malloc(MAX_PATH * PATH_COMPARISON_BUFFER_COUNT);
  if (buffers == (char*)0){
    return -1;
  }

  char* cwd = buffers;
  char* normalized_left = buffers + MAX_PATH;
  char* normalized_right = normalized_left + MAX_PATH;
  char* cwd_for_comparison = (char*)0;
  if (left[0] != '/' || right[0] != '/'){
    if (getcwd(cwd, MAX_PATH) == (char*)-1){
      free(buffers);
      return -1;
    }
    cwd_for_comparison = cwd;
  }

  if (!normalize_path_for_comparison(left, cwd_for_comparison,
                                     normalized_left) ||
      !normalize_path_for_comparison(right, cwd_for_comparison,
                                     normalized_right)){
    free(buffers);
    return -1;
  }

  int equal = streq(normalized_left, normalized_right) ? 1 : 0;
  free(buffers);
  return equal;
}

// Read and print directory entries for the shell's `ls` implementation.
void list_dir(char* path, bool print_header, bool is_last_dir, char* command) {
  struct LinkedDirent* entries = read_directory(path);
  if (entries == (struct LinkedDirent*) -1) {
    int args[2] = {(int) command, (int) path};
    printf("%s: cannot access directory '%s'\n", args);
    return;
  }

  if (print_header) {
    int args[1] = {(int) path};
    // Restore white in the header itself. print_directory intentionally emits
    // nothing for an all-filtered directory, so it cannot own this reset.
    printf("\x1b[51m%s:\x1b[37m\n", args);
  }

  print_directory(entries, true);
  destroy_linked_dirents(entries);

  if (!is_last_dir) {
    puts("\n");
  }
}

// Parse and execute the shell's directory-listing command.
void handle_ls(int argc, char** argv) {
  if (argc == 1) {
    // List current directory.
    list_dir(".", false, true, "ls");
  } else {
    // Only show path if multiple directories.
    bool print_header = argc > 2;
    for (int i = 1; i < argc; i++) {
      list_dir(argv[i], print_header, i == argc - 1, "ls");
    }
  }
}

// Open each named file and write its contents to standard output.
void handle_cat(int argc, char** argv){
  if (argc < 2){
    puts("cat: expected file argument\n");
  } else {
    int fd = open_existing(argv[1]);
    if (fd < 0){
      puts("cat: failed to open file\n");
    } else {
      // read all bytes from file and write to STDOUT
      char buffer[1024];
      int bytes_read;
      while ((bytes_read = read(fd, buffer, 1024)) > 0){
        write(STDOUT, buffer, (unsigned)bytes_read);
      }
      if (bytes_read < 0){
        puts("cat: failed to read file\n");
      }
      close(fd);
    }
  }
}

// Copy bytes from the source file into the destination file.
bool handle_cp(int argc, char** argv){
  // copy file: cp source dest
  if (argc < 3){
    puts("cp: expected source and destination arguments\n");
    return false;
  }

  int path_comparison = compare_lexical_paths(argv[1], argv[2]);
  if (path_comparison < 0){
    puts("cp: failed to safely normalize source and destination paths\n");
    return false;
  }

  // This catches lexical aliases such as `file`, `./file`, and paths that
  // differ only by repeated separators or dot components. Symlink and hard-
  // link identity still requires kernel descriptor-identity and rename APIs;
  // until those exist, mv cannot reject every possible alias atomically.
  if (path_comparison > 0){
    puts("cp: source and destination resolve to the same lexical path\n");
    return false;
  }

  /*
   * A final-component symlink can alias the source even though its pathname is
   * lexically different. In particular, allowing `mv file link-to-file` would
   * copy through the link and then unlink the only regular-file name, leaving
   * a dangling link. A one-byte readlink buffer is sufficient because only the
   * success status matters; every symlink target contributes at least its NUL.
   *
   * This is deliberately a conservative, non-atomic guard. The current ABI has
   * no descriptor identity or rename operation, so hard links, symlinks in an
   * earlier path component, and replacement after this probe remain unresolved.
   */
  char destination_symlink_probe;
  if (readlink(argv[2], &destination_symlink_probe,
               sizeof(destination_symlink_probe)) >= 0){
    int args[1] = {(int)argv[2]};
    printf("cp: refusing destination '%s': final path component is a symbolic link\n",
           args);
    return false;
  }
    
  int src_fd = open_existing(argv[1]);
  if (src_fd < 0){
    puts("cp: failed to open source file\n");
    return false;
  } 
  
  int dest_fd = open(argv[2]);
  if (dest_fd < 0){
    puts("cp: failed to open destination file\n");
    close(src_fd);
    return false;
  }

  // Read all source bytes and copy them to the destination. write() may
  // complete only part of a request, so every chunk must be drained before
  // reading the next one.
  char buffer[1024];
  int bytes_read = 0;
  bool success = true;
  while (success && (bytes_read = read(src_fd, buffer, 1024)) > 0){
    unsigned written = 0;
    while (written < (unsigned)bytes_read){
      int write_result = write(dest_fd, buffer + written,
                               (unsigned)bytes_read - written);
      if (write_result <= 0){
        puts("cp: destination write failed or made no progress\n");
        success = false;
        break;
      }
      written += (unsigned)write_result;
    }
  }

  if (success && bytes_read < 0){
    puts("cp: failed to read source file\n");
    success = false;
  }

  // Truncate only after a complete copy. This removes any old destination
  // tail while ensuring a failed copy can never be reported as successful.
  if (success){
    int source_size = seek(src_fd, 0, SEEK_END);
    if (source_size < 0){
      puts("cp: failed to determine source file size\n");
      success = false;
    } else if (truncate(dest_fd, (unsigned)source_size) != 0){
      puts("cp: failed to truncate destination file\n");
      success = false;
    }
  }

  // Both descriptors remain owned by this function on every path after both
  // opens succeed. A close failure also means mv must preserve its source.
  if (close(src_fd) != 0){
    puts("cp: failed to close source file\n");
    success = false;
  }
  if (close(dest_fd) != 0){
    puts("cp: failed to close destination file\n");
    success = false;
  }

  return success;
}

// Parse one command line and dispatch its builtin or external command.
void handle_command(void){
  unsigned argc;
  char** argv;
  parse_command(&argc, &argv);

  // check if command is one of our built-in commands
  if (argc == 0){
    // empty command, do nothing
  } else if (streq(argv[0], "cd")){
    // change directory
    if (argc < 2){
      puts("cd: expected path argument\n");
    } else if (chdir(argv[1]) != 0){
      puts("cd: failed to change directory\n");
    }
  } else if (streq(argv[0], "ls")){
    handle_ls(argc, argv);
  } else if (streq(argv[0], "cat")){
    handle_cat(argc, argv);
  } else if (streq(argv[0], "clear")){
    // clear screen by printing ANSI escape code
    puts("\x1b[2J\x1b[H");
  } else if (streq(argv[0], "exit")){
    exit(0);
  } else if (streq(argv[0], "cp")){
    handle_cp(argc, argv);
  } else if (streq(argv[0], "mkdir")){
    if (argc < 2){
      puts("mkdir: expected path argument\n");
    } else if (mkdir(argv[1]) != 0){
      puts("mkdir: failed to create directory\n");
    }
  } else if (streq(argv[0], "rm")){
    if (argc < 2){
      puts("rm: expected path argument\n");
    } else if (unlink(argv[1]) != 0){
      puts("rm: failed to remove file\n");
    }
  } else if (streq(argv[0], "rmdir")){
    if (argc < 2){
      puts("rmdir: expected path argument\n");
    } else if (rmdir(argv[1]) != 0){
      puts("rmdir: failed to remove directory\n");
    }
  } else if (streq(argv[0], "mv")){
    if (argc < 3){
      puts("mv: expected source and destination arguments\n");
    }
    // implement mv as cp + rm
    else {
      if (!handle_cp(argc, argv)){
        puts("mv: copy failed; source file was not removed\n");
      } else if (unlink(argv[1]) != 0){
        puts("mv: failed to remove source file after copying\n");
      }
    }
  } else if (streq(argv[0], "help")){
    puts("built-in commands:\n");
    puts("  cd [path] - change current directory\n");
    puts("  ls - list entries in current directory\n");
    puts("  cat [file] - print contents of file to terminal\n");
    puts("  cp [source] [dest] - copy file from source to dest\n");
    puts("  mv [source] [dest] - move file from source to dest\n");
    puts("  rm [file] - remove file\n");
    puts("  mkdir [path] - create directory at path\n");
    puts("  rmdir [path] - remove directory at path (must be empty)\n");
    puts("  clear - clear the terminal screen\n");
    puts("  exit - exit the shell\n");
    puts("  help - print this help message\n");
    puts("  ^C - cancel current command\n");
  } else {
    // Keep the child from entering a program that can touch VGA until the
    // parent has installed the corresponding foreground descriptor. Without
    // this gate, the child can win the post-fork scheduling race and its first
    // display trap cannot yet be attributed to the foreground job.
    int start_gate = sem_open(FOREGROUND_START_GATE_CLOSED);
    if (start_gate < 0){
      puts("shell: failed to create foreground-command start gate\n");
    } else {
      // exec other command
      int pid = fork();
      if (pid < 0){
        puts("shell: failed to fork external command\n");
        sem_close(start_gate);
      } else if (pid == 0){
        if (sem_down(start_gate) != 0){
          puts("shell child: failed to wait for foreground installation\n");
          exit(1);
        }
        sem_close(start_gate);

        // Look for argv[0] in /sbin/ first. The shell command buffer is larger
        // than MAX_PATH, so do not concatenate a command that cannot fit in a
        // valid kernel pathname. If /sbin is inapplicable or exec fails, try
        // the command exactly as entered relative to the current directory.
        unsigned command_length = strlen(argv[0]);
        unsigned prefix_length = strlen(SBIN_PREFIX);
        if (prefix_length < MAX_PATH &&
            command_length < MAX_PATH - prefix_length){
          unsigned path_size = prefix_length + command_length + 1;
          char* path_buf = malloc(path_size);
          memcpy(path_buf, SBIN_PREFIX, prefix_length);
          memcpy(path_buf + prefix_length, argv[0], command_length + 1);
          execv(path_buf, argc, argv);
          free(path_buf);
        }
        if (execv(argv[0], argc, argv) != 0){
          puts("shell child: failed to exec external command\n");
        }
        exit(1);
      } else {
        int foreground_set = set_foreground_child(pid);
        if (foreground_set != 0){
          puts("shell: failed to install external command as foreground child\n");
        }

        // The child owns another reference to the semaphore descriptor, so it
        // remains valid after the parent releases the gate and closes its copy.
        sem_up(start_gate);
        sem_close(start_gate);
        wait_child(pid);

        if (foreground_set == 0){
          // Clearing the foreground slot returns whether that child used a
          // direct display trap. Queue recovery through the terminal pipe so
          // the terminal resets both VGA hardware and its renderer state
          // before the next prompt.
          if (set_foreground_child(-1) > 0){
            puts(TERMINAL_RESET_DISPLAY_SEQUENCE);
          }
        }
      }
    }
  }

  // Discard the parsed command name and arguments after dispatch.
  free_argv(argc, argv);
}

// Focused shell tests compile the command handlers into a guest test program;
// production builds leave SHELL_LIBRARY_ONLY undefined and use this entrypoint.
#ifndef SHELL_LIBRARY_ONLY
// Read and dispatch commands from the interactive shell prompt.
int main(void){
  while (true) { 
    print_line_prefix();

    while (true){
      char key;
      if (read(STDIN, &key, 1) != 1){
        sleep(1);
        continue;
      }

      if (key == '\n' || key == '\r'){
        putchar('\n');
        handle_command();
        cmd_buf_len = 0;
        break;
      } else if (key == '\t') {
        // TODO: if allowing cursor to move, either tab-complete or ignore if in middle.

        // Tab-completion.
        int start = 0;
        for (int i = cmd_buf_len - 1; i >= 0; i--) {
          if (cmd_buf[i] == ' ') {
            start = i + 1;
            break;
          }
        }

        char* to_tab_complete = malloc(cmd_buf_len - start + 1);
        memcpy(to_tab_complete, &cmd_buf[start], cmd_buf_len - start);
        to_tab_complete[cmd_buf_len - start] = 0;

        int last_slash = start;
        for (int i = cmd_buf_len - 1; i >= start; i--) {
          if (cmd_buf[i] == '/') {
            last_slash = i + 1;
            break;
          }
        }

        // Completion with paths.
        struct LinkedDirent* matches = tab_complete_directory(to_tab_complete, last_slash == 0);
        int num_matches = 0;
        for (struct LinkedDirent* current = matches; current != 0; current = current->next) {
          num_matches++;
        }

        // Exclude ".", "..", and "lost+found" if matches <= 3, and first character is not '.'.
        if (num_matches <= 3 && to_tab_complete[last_slash - start] != '.') {
          num_matches = 0;
          struct LinkedDirent* filtered_matches = 0;
          struct LinkedDirent* filtered_tail = 0;
          for (struct LinkedDirent* current = matches; current != 0; current = current->next) {
            char* name = &current->dirent.d_name;
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
              // Skip.
              continue;
            }
            num_matches++;
            struct LinkedDirent* new_entry = create_linked_dirent(&current->dirent);
            if (filtered_matches == 0) {
              filtered_matches = new_entry;
              filtered_tail = new_entry;
            } else {
              filtered_tail->next = new_entry;
              filtered_tail = new_entry;
            }
          }
          destroy_linked_dirents(matches);
          matches = filtered_matches;
        }

        if (num_matches != 0) { // Only do something if match.
          // Find longest common prefix.
          int prefix_length = cmd_buf_len - last_slash;
          while (1) {
            char c = 0;
            struct LinkedDirent* current = matches;
            while (current != 0) {
              char* name = &current->dirent.d_name;
              if (name[prefix_length] == 0) {
                // End of this name.
                c = 0;
                break;
              }
              if (c == 0) {
                c = name[prefix_length];
              } else if (name[prefix_length] != c) {
                // Mismatch.
                c = 0;
                break;
              }
              current = current->next;
            }
            if (c == 0) {
              // Mismatch found (or end).
              break;
            } else {
              // All shared this character.
              prefix_length++;
            }
          }

          int new_characters = prefix_length - (cmd_buf_len - last_slash);
          for (int i = 0; i < new_characters &&
                          cmd_buf_len < SHELL_COMMAND_BUFFER_SIZE - 1; i++) {
            char add_c = (&matches->dirent.d_name)[cmd_buf_len - last_slash];
            char str[2] = {add_c, '\0'};
            puts(str);
            cmd_buf[cmd_buf_len++] = add_c;
          }
          // If at end of only one match, add space or '/'.
          if (num_matches == 1) {
            if (cmd_buf_len < SHELL_COMMAND_BUFFER_SIZE - 1) {
              char add_c;
              if (matches->d_type == DT_DIR) {
                add_c = '/';
              } else {
                add_c = ' ';
              }
              char str[2] = {add_c, '\0'};
              puts(str);
              cmd_buf[cmd_buf_len++] = add_c;
            }
          } else if (new_characters == 0) { // No new characters.
            puts("\n");
            print_directory(matches, to_tab_complete[last_slash - start] != '.');

            // Reprint prompt and command.
            print_line_prefix();
            print_cmd_buf();
          }
        }
        free(to_tab_complete);
        destroy_linked_dirents(matches);
      } else if (key == 127 || key == 8){
        // backspace
        if (cmd_buf_len > 0){
          cmd_buf_len--;
          puts("\b \b");
        }
      } else if (key == 0x03) {
        // Cancel current command.
        puts("^C\n");
        cmd_buf_len = 0;
        break;
      } else if (key >= 32 && key < 127){
        // printable character
        if (cmd_buf_len < SHELL_COMMAND_BUFFER_SIZE - 1){
          cmd_buf[cmd_buf_len++] = key;
          char str[2] = {key, '\0'};
          puts(str);
        }
      }
    }
  }

  return 0;
}
#endif
