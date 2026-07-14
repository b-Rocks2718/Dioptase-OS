#include "../crt/print.h"
#include "../crt/sys.h"
#include "../crt/stdbool.h"
#include "../crt/ctype.h"
#include "../crt/string.h"
#include "../crt/stdlib.h"
#include "../crt/fcntl.h"
#include "../crt/unistd.h"
#include "../crt/ps2.h"
#include "../crt/sys/wait.h"

#include "dirs.h"

#define CMD_BUF_SIZE 2048
#define MAX_ARGV 16

char cmd_buf[CMD_BUF_SIZE];
unsigned cmd_buf_len = 0;
bool shift_held = false;

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

void print_cmd_buf(void){
  cmd_buf[cmd_buf_len] = '\0';
  puts(cmd_buf);
}

static char apply_shift_to_char(char c){
  if (isalpha(c)){
    if (c >= 'a' && c <= 'z'){
      return c - 'a' + 'A';
    }
    return c;
  }

  if (c == '0'){
    return ')';
  } else if (c == '1'){
    return '!';
  } else if (c == '2'){
    return '@';
  } else if (c == '3'){
    return '#';
  } else if (c == '4'){
    return '$';
  } else if (c == '5'){
    return '%';
  } else if (c == '6'){
    return '^';
  } else if (c == '7'){
    return '&';
  } else if (c == '8'){
    return '*';
  } else if (c == '9'){
    return '(';
  } else if (c == '-'){
    return '_';
  } else if (c == '='){
    return '+';
  } else if (c == '['){
    return '{';
  } else if (c == ']'){
    return '}';
  } else if (c == '\\'){
    return '|';
  } else if (c == ';'){
    return ':';
  } else if (c == '\''){
    return '"';
  } else if (c == ','){
    return '<';
  } else if (c == '.'){
    return '>';
  } else if (c == '/'){
    return '?';
  } else if (c == '`'){
    return '~';
  }

  return c;
}

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

void free_argv(unsigned argc, char** argv){
  for (unsigned i = 0; i < argc; i++){
    free(argv[i]);
  }
  free(argv);
}

void list_dir(char* path, bool print_header, bool is_last_dir, char* command) {
  struct LinkedDirent* entries = read_directory(path);
  if (entries == (struct LinkedDirent*) -1) {
    int args[2] = {(int) command, (int) path};
    printf("%s: cannot access directory '%s'\n", args);
    return;
  }

  if (print_header) {
    int args[1] = {(int) path};
    printf("\x1b[51m%s:\n", args);
  }

  print_directory(entries, true);
  destroy_linked_dirents(entries);

  if (!is_last_dir) {
    puts("\n");
  }
}

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

void handle_cat(int argc, char** argv){
  if (argc < 2){
    puts("cat: expected file argument\n");
  } else {
    int fd = open(argv[1]);
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

void handle_cp(int argc, char** argv){
  // copy file: cp source dest
  if (argc < 3){
    puts("cp: expected source and destination arguments\n");
    return;
  } 
    
  int src_fd = open(argv[1]); 
  if (src_fd < 0){
    puts("cp: failed to open source file\n");
    return;
  } 
  
  int dest_fd = open(argv[2]);
  if (dest_fd < 0){
    puts("cp: failed to open destination file\n");
    close(src_fd);
    return;
  }

  // ready in all source bytes and copy to dest
  char buffer[1024];
  int bytes_read;
  while ((bytes_read = read(src_fd, buffer, 1024)) > 0){
    if (write(dest_fd, buffer, (unsigned)bytes_read) < 0){
      puts("cp: failed to write to destination file\n");
      return;
    }
  }
  if (bytes_read < 0){
    puts("cp: failed to read source file\n");
    return;
  }

  // truncate dest to source size
  if (truncate(dest_fd, seek(src_fd, 0, SEEK_END)) != 0){
    puts("cp: failed to truncate destination file\n");
    return;
  }

  close(src_fd);
  close(dest_fd);
}

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
      handle_cp(argc, argv);
      if (unlink(argv[1]) != 0){
        puts("mv: failed to remove source file after copying\n");
      }
    }
  } else if (streq(argv[0], "help")){
    puts("built-in commands:\n");
    puts("cd [path] - change current directory\n");
    puts("ls - list entries in current directory\n");
    puts("cat [file] - print contents of file to terminal\n");
    puts("cp [source] [dest] - copy file from source to dest\n");
    puts("mv [source] [dest] - move file from source to dest\n");
    puts("rm [file] - remove file\n");
    puts("mkdir [path] - create directory at path\n");
    puts("rmdir [path] - remove directory at path (must be empty)\n");
    puts("clear - clear the terminal screen\n");
    puts("exit - exit the shell\n");
  } else {
    // exec other command
    int pid = fork();
    if (pid < 0){
      puts("failed to fork process\n");
    } else if (pid == 0){
      // look for argv[0] in /sbin/ first, then in current directory
      char* path_buf = malloc(MAX_PATH);
      memcpy(path_buf, "/sbin/", 6);
      memcpy(path_buf + 6, argv[0], strlen(argv[0]) + 1);
      if (execv(path_buf, argc, argv) != 0){
        // exec failed, try current directory
        if (execv(argv[0], argc, argv) != 0){
          puts("failed to exec command\n");
        }
      }
      free(path_buf);
      exit(1);
    } else {
      wait_child(pid);
    }
  }

  // free argv
  free_argv(argc, argv);
}

int main(void){
  while (true) { 
    print_line_prefix();

    while (true){
      short key = getkey();
      if (key & 0xFF00){
        key = key & 0xFF;
        if (key == KEY_LEFT_SHIFT || key == KEY_RIGHT_SHIFT){
          shift_held = false;
        }
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
          for (int i = 0; i < new_characters && cmd_buf_len < CMD_BUF_SIZE - 1; i++) {
            char add_c = (&matches->dirent.d_name)[cmd_buf_len - last_slash];
            char str[2] = {add_c, '\0'};
            puts(str);
            cmd_buf[cmd_buf_len++] = add_c;
          }
          // If at end of only one match, add space or '/'.
          if (num_matches == 1) {
            if (cmd_buf_len < CMD_BUF_SIZE - 1) {
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
            // Print matches.
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
      } else if (key == KEY_LEFT_SHIFT || key == KEY_RIGHT_SHIFT){
        shift_held = true;
      } else if (key >= 32 && key < 127){
        // printable character
        if (cmd_buf_len < CMD_BUF_SIZE - 1){
          char typed = key;
          if (shift_held){
            typed = apply_shift_to_char(typed);
          }

          cmd_buf[cmd_buf_len++] = typed;
          char str[2] = {typed, '\0'};
          puts(str);
        }
      }

      sleep(5);
    }
  }

  return 0;
}
