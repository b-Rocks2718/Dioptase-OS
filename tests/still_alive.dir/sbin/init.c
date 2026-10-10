/*
 * Manual VGA/audio integration exercise for the bundled Still Alive demo.
 * The lyrics buffer always reserves and installs a trailing NUL so a file at
 * the read limit cannot make the dramatic printer scan beyond the buffer.
 *
 * Audio is streamed with root/crt/wav.h: this process owns the audio device
 * and must keep its ~0.33 s ring fed, so every pause pumps the stream one
 * jiffy at a time instead of sleeping in one long call.
 */

#include "../../../root/crt/sys.h"
#include "../../../root/crt/print.h"
#include "../../../root/crt/wav.h"

struct WavStream music;

void sleep_pumping(unsigned jiffies){ /* Sleep while keeping the audio ring fed. */
  for (unsigned i = 0; i < jiffies; i++){
    wav_stream_pump(&music);
    sleep(1);
  }
}

void print_dramatically(char* str, unsigned delay){ /* Print one character per delay interval, treating '%' as a longer silent pause. */
  for (unsigned i = 0; str[i] != '\0'; i++){
    if (str[i] == '%'){
      sleep_pumping(delay * 10);
      continue;
    }
    putchar(str[i]);
    sleep_pumping(delay);
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

  struct WavError error;
  if (wav_stream_open(&music, "still_alive.wav", &error) != WAV_OK){
    wav_print_error("still_alive test", "still_alive.wav", &error);
    return -1;
  }

  print_dramatically("Initializing GLaDOS", DELAY);
  sleep_pumping(50);
  putchar('.');
  sleep_pumping(50);
  putchar('.');
  sleep_pumping(50);
  putchar('.');
  putchar('\n');
  putchar('\n');
  sleep_pumping(20);

  print_dramatically(lyrics, DELAY);

  // Let the rest of the song play out before releasing the device.
  while (!wav_stream_finished(&music)){
    sleep_pumping(1);
  }
  wav_stream_close(&music);

  return 0;
}
