#include "../../crt/print.h"
#include "../../crt/sys.h"
#include "../../crt/constants.h"
#include "../../crt/debug.h"

int main(void) {
  puts("| Hello from init process!\n");

  // close the debug stdio pipes
  // and replace with our own
  int stdout_pipe[2] = {-1, -1};
  pipe(stdout_pipe);
  close(STDOUT);
  assert(dup(stdout_pipe[1]) == STDOUT, "| failed to dup stdout pipe\n");
  close(stdout_pipe[1]);

  // write end of stdout pipe is now assigned to STDOUT

  // create terminal emulator process
  int terminal_pid = fork();
  if (terminal_pid < 0) {
    // this probably never prints bc we have no terminal yet
    puts("| failed to fork terminal emulator process\n");
    return -1;
  } else if (terminal_pid == 0) {
    // replace STDIN with the read end of the stdout pipe
    close(STDIN);
    assert(dup(stdout_pipe[0]) == STDIN, "| failed to dup stdout pipe read end to STDIN\n");
    close(stdout_pipe[0]);

    // child process: exec terminal emulator
    execv("/sbin/terminal", 0, NULL);

    // this probably never prints bc we have no terminal yet
    puts("|failed to exec terminal emulator\n");
    return -1;
  }

  // close read end of stdout pipe in parent
  close(stdout_pipe[0]);

  // start shell
  int shell_pid = fork();
  if (shell_pid < 0) {
    puts("| failed to fork shell process\n");
    return -1;
  } else if (shell_pid == 0) {
    execv("/sbin/shell", 0, NULL);
    puts("| failed to exec shell\n");
    return -1;
  }

  wait_child(shell_pid);

  return 67;
}
