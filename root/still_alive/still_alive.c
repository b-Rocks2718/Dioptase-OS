#include "../crt/sys.h"
#include "../crt/print.h"

// Print text one character at a time, treating '%' as a longer pause.
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
#define LYRICS_BUFFER_BYTES 2048

char lyrics[LYRICS_BUFFER_BYTES];

// Load the lyrics and animate them on the terminal with timed pauses.
int main(void){
  // clear the screen
  puts("\x1b[2J");

  // set text color to light orange
  puts("\x1b[47m");

  // home cursor
  puts("\x1b[H");

  int lyrics_fd = open_existing("/still_alive/lyrics.txt");
  if (lyrics_fd < 0){
    puts("still_alive: could not open /still_alive/lyrics.txt\n");
    return -1;
  }

  // Reserve one byte for the terminator consumed by print_dramatically().
  // read() returns raw bytes and does not append a terminator when the file
  // fills the caller's buffer.
  int lyrics_bytes = read(lyrics_fd, lyrics, sizeof(lyrics) - 1);
  if (lyrics_bytes < 0){
    close(lyrics_fd);
    puts("still_alive: could not read /still_alive/lyrics.txt\n");
    return -1;
  }
  lyrics[lyrics_bytes] = '\0';

  if (close(lyrics_fd) < 0){
    puts("still_alive: could not close /still_alive/lyrics.txt\n");
    return -1;
  }

  int music_fd = open_existing("/still_alive/still_alive.wav");
  if (music_fd < 0){
    puts("still_alive: could not open /still_alive/still_alive.wav\n");
    return -1;
  }
  if (play_audio_file(music_fd) < 0){
    close(music_fd);
    puts("still_alive: could not start /still_alive/still_alive.wav\n");
    return -1;
  }
  if (close(music_fd) < 0){
    puts("still_alive: could not close /still_alive/still_alive.wav\n");
    return -1;
  }

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
