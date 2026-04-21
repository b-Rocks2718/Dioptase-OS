#include "../../crt/print.h"
#include "../../crt/sys.h"
#include "../../crt/constants.h"
#include "../../crt/debug.h"
#include "../../crt/string.h"
#include "../../crt/heap.h"
#include "../../crt/dirent.h"
#include "../../crt/ps2.h"

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
    // print entries in current directory
    int fd = open(".");
    if (fd < 0){
      puts("ls: failed to open current directory\n");
    } else {
      char* buffer = malloc(1024);
      int bytes_read = getdents(fd, buffer, 1024);
      if (bytes_read < 0){
        puts("ls: failed to read directory entries\n");
      } else {
        // print each entry name followed by newline
        unsigned offset = 0;
        while (offset < (unsigned)bytes_read){
          struct linux_dirent* entry = (struct linux_dirent*)(buffer + offset);
          puts(&entry->d_name);
          puts("\n");
          offset += entry->d_reclen;
        }
      }
      free(buffer);
      close(fd);
    }
  } else if (streq(argv[0], "cat")){
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
  } else if (streq(argv[0], "clear")){
    // clear screen by printing ANSI escape code
    puts("\x1b[2J\x1b[H");
  } else if (streq(argv[0], "exit")){
    exit(0);
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
