#include "../crt/sys.h"
#include "../crt/print.h"

void print_dramatically(char* str, unsigned delay){
  for (unsigned i = 0; str[i] != '\0'; i++){
    if (str[i] == '%'){
      sleep(delay * 10);
      continue;
    }
    putchar(str[i]);
    sleep(delay);
  }
}

#define DELAY 12

char lyrics[2048];

int main(void){
  // clear the screen
  puts("\x1b[2J");

  // set text color to light orange
  puts("\x1b[47m");

  // home cursor
  puts("\x1b[H");

  int lyrics_fd = open("/still_alive/lyrics.txt");
  read(lyrics_fd, lyrics, sizeof(lyrics));
  close(lyrics_fd);

  int music_fd = open("/still_alive/still_alive.wav");
  play_audio_file(music_fd);

  print_dramatically("Initializing GLaDOS", DELAY);
  sleep(50);
  putchar('.');
  sleep(50);
  putchar('.');
  sleep(50);
  putchar('.');
  putchar('\n');
  putchar('\n');
  sleep(20);

  print_dramatically(lyrics, DELAY);

  // reset text color
  puts("\x1b[0m");

  // home cursor
  puts("\x1b[H");

  // clear the screen
  puts("\x1b[2J");

  return 0;
}
