#include "../kernel/config.h"
#include "../kernel/audio.h"
#include "../kernel/debug.h"
#include "../kernel/print.h"
#include "../kernel/scheduler.h"
#include "../kernel/threads.h"
#include "../kernel/heap.h"
#include "../kernel/ext.h"
#include "../kernel/vmem.h"

void audio_worker(void* unused){
  struct AudioWav wav;
  unsigned num_samples;
  unsigned preview_count;

  set_priority(HIGH_PRIORITY);

  audio_wav_load_from_root_or_panic("still_alive.wav", &wav);

  if (!CONFIG.use_audio){
    say("Host audio disabled; run EMU_AUDIO_FAST=yes make still_alive to hear playback\n",
      NULL);
    return;
  }

  audio_wav_play_blocking(&wav);
}

void say_dramatically(char* str, unsigned color, unsigned delay){
  for (unsigned i = 0; str[i] != '\0'; i++){
    if (str[i] == '%'){
      sleep(delay * 10);
      continue;
    }
    putchar_color(str[i], color);
    sleep(delay);
  }
}

#define WAV_SAMPLE_PREVIEW_COUNT 16

int kernel_main(void){

  clear_screen();

  struct Node* lyrics_node = node_find(&fs.root, "lyrics.txt");
  char* lyrics = mmap(node_size_in_bytes(lyrics_node), lyrics_node, 0, MMAP_READ);
  node_free(lyrics_node);

  struct Fun* audio_worker_fun = malloc(sizeof(struct Fun));
  audio_worker_fun->func = audio_worker;
  audio_worker_fun->arg = NULL;
  thread_(audio_worker_fun, HIGH_PRIORITY, ANY_CORE);

  say_dramatically("Initializing GLaDOS", 0xF9, 2);
  sleep(80);
  putchar_color('.', 0xF9);
  sleep(80);
  putchar_color('.', 0xF9);
  sleep(80);
  putchar_color('.', 0xF9);
  putchar_color('\n', 0xF9);
  putchar_color('\n', 0xF9);
  sleep(50);

  say_dramatically(lyrics, 0xF9, 3);

  munmap(lyrics);

  return 0;
}
