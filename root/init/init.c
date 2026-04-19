#include "../../crt/print.h"
#include "../../crt/sys.h"
#include "../../crt/constants.h"
#include "../../crt/debug.h"

int main(void) {
  puts("***hello from init process!\n");

  // set up stdio pipes
  int stdin_pipe[2];
  pipe(stdin_pipe);

  int stdout_pipe[2];
  pipe(stdout_pipe);

  int stderr_pipe[2];
  pipe(stderr_pipe);

  // close kernel debug stdio pipes and replace them with our own
  close(STDIN);
  assert(dup(stdin_pipe[0]) == STDIN, "***failed to dup stdin pipe\n");
  close(STDOUT);
  assert(dup(stdout_pipe[1]) == STDOUT, "***failed to dup stdout pipe\n");
  close(STDERR);
  assert(dup(stderr_pipe[1]) == STDERR, "***failed to dup stderr pipe\n");

  // close the extra pipes created by this process
  close(stdin_pipe[0]);
  close(stdout_pipe[1]);
  close(stderr_pipe[1]);

  // create terminal emulator process
  int pid = fork();
  if (pid < 0) {
    puts("***failed to fork terminal emulator process\n");
    return 1;
  } else if (pid == 0) {
    // child process: exec terminal emulator
    char* terminal_argv[1] = {"terminal"};
    execv("/sbin/terminal", 1, terminal_argv);
    puts("***failed to exec terminal emulator\n");
    return 1;
  }

  if (wait_child(pid) < 0) {
    puts("***failed to wait for terminal emulator process\n");
    return 1;
  }

  return 67;
}
