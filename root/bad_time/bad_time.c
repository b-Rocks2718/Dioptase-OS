#include "../crt/sys.h"
#include "../crt/stdio.h"

#define DELAY 12

char art[4096];

int main(void){
  int id = fork();
  if (id < 0){
    puts("fork failed");
    return 1;
  } else if (id == 0){
    // child process execs `synth` to play the music
    char* args[2];
    args[0] = "synth";
    args[1] = "/bad_time/bad_time.dsyn";
    execv("/sbin/synth", 2, args);
    puts("exec failed");
    return 1;
  }

  sleep(1000);

  // print ascii art
  int art_fd = open("/bad_time/sans.txt");
  unsigned chars_read = 0;
  unsigned offset = 0;
  do {
    chars_read = read(art_fd, art + offset, sizeof(art) - offset - 1);
    offset += chars_read;
  } while (chars_read > 0 && offset < sizeof(art) - 1);
  close(art_fd);

  puts(art);

  // exit on 'q' key press
  while (1){
    char c;
    if (read(STDIN, &c, 1) > 0){
      if (c == 'q' || c == 'Q'){
        break;
      }
    }
    sleep(10);
  }

  // kill the child process
  signal_child(id, DIOPTASE_SIGNAL_TERMINATE);

  // reset text color
  puts("\x1b[0m");

  // home cursor
  puts("\x1b[H");

  // clear the screen
  puts("\x1b[2J");

  return 0;
}
