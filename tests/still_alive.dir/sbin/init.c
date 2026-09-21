/*
 * Manual VGA/audio integration exercise for the bundled Still Alive demo.
 * The lyrics buffer always reserves and installs a trailing NUL so a file at
 * the read limit cannot make the dramatic printer scan beyond the buffer.
 */

#include "../../../root/crt/sys.h"
#include "../../../root/crt/print.h"

void print_dramatically(char* str, unsigned delay){ /* Print one character per delay interval, treating '%' as a longer silent pause. */
  for (unsigned i = 0; str[i] != '\0'; i++){
    if (str[i] == '%'){
      sleep(delay * 10);
      continue;
    }
    putchar(str[i]);
    sleep(delay);
  }
}

#define DELAY 3
#define LYRICS_BUFFER_BYTES 2048

char lyrics[LYRICS_BUFFER_BYTES];

int main(void){ /* Keep a user process alive while the harness checks scheduling. */
  clear_screen();

  set_text_color(0xF9);

  int lyrics_fd = open("lyrics.txt");
  if (lyrics_fd < 0){
    puts("still_alive test: could not open lyrics.txt\n");
    return -1;
  }

  int lyrics_bytes = read(lyrics_fd, lyrics, sizeof(lyrics) - 1);
  if (lyrics_bytes < 0){
    close(lyrics_fd);
    puts("still_alive test: could not read lyrics.txt\n");
    return -1;
  }
  lyrics[lyrics_bytes] = '\0';

  if (close(lyrics_fd) < 0){
    puts("still_alive test: could not close lyrics.txt\n");
    return -1;
  }

  int music_fd = open("still_alive.wav");
  if (music_fd < 0){
    puts("still_alive test: could not open still_alive.wav\n");
    return -1;
  }
  if (play_audio_file(music_fd) < 0){
    close(music_fd);
    puts("still_alive test: could not start still_alive.wav\n");
    return -1;
  }
  if (close(music_fd) < 0){
    puts("still_alive test: could not close still_alive.wav\n");
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

  return 0;
}
