/*
 * user_misc_syscalls guest:
 * - validate the user-visible wrappers for jiffies, sleep, getkey, semaphore,
 *   mmap, and play_audio_file
 * - ensure semaphore exhaustion returns -1 instead of dereferencing an invalid
 *   descriptor slot
 * - generate a tiny valid WAV file in-place so play_audio_file can take a real
 *   success path without needing a checked-in binary fixture
 */

#include "../../../crt/sys.h"

#define TEST_WAV_BYTES 46

static void fill_test_wav(char* wav_bytes){
  wav_bytes[0] = 'R';
  wav_bytes[1] = 'I';
  wav_bytes[2] = 'F';
  wav_bytes[3] = 'F';
  wav_bytes[4] = 38;
  wav_bytes[5] = 0;
  wav_bytes[6] = 0;
  wav_bytes[7] = 0;
  wav_bytes[8] = 'W';
  wav_bytes[9] = 'A';
  wav_bytes[10] = 'V';
  wav_bytes[11] = 'E';
  wav_bytes[12] = 'f';
  wav_bytes[13] = 'm';
  wav_bytes[14] = 't';
  wav_bytes[15] = ' ';
  wav_bytes[16] = 16;
  wav_bytes[17] = 0;
  wav_bytes[18] = 0;
  wav_bytes[19] = 0;
  wav_bytes[20] = 1;
  wav_bytes[21] = 0;
  wav_bytes[22] = 1;
  wav_bytes[23] = 0;
  wav_bytes[24] = (char)0xA8;
  wav_bytes[25] = 0x61;
  wav_bytes[26] = 0;
  wav_bytes[27] = 0;
  wav_bytes[28] = 0x50;
  wav_bytes[29] = (char)0xC3;
  wav_bytes[30] = 0;
  wav_bytes[31] = 0;
  wav_bytes[32] = 2;
  wav_bytes[33] = 0;
  wav_bytes[34] = 16;
  wav_bytes[35] = 0;
  wav_bytes[36] = 'd';
  wav_bytes[37] = 'a';
  wav_bytes[38] = 't';
  wav_bytes[39] = 'a';
  wav_bytes[40] = 2;
  wav_bytes[41] = 0;
  wav_bytes[42] = 0;
  wav_bytes[43] = 0;
  wav_bytes[44] = 0;
  wav_bytes[45] = 0;
}

int main(void){
  int sems[100];
  int sem;
  char wav_bytes[TEST_WAV_BYTES];
  unsigned* anon;
  char* file_map;

  fill_test_wav(wav_bytes);

  test_syscall(getkey());

  unsigned start = get_current_jiffies();
  sleep(3);
  test_syscall(get_current_jiffies() - start >= 3);

  yield();
  test_syscall(1);

  test_syscall(sem_open(-1));

  sem = sem_open(1);
  test_syscall(sem >= 100);
  test_syscall(sem_down(sem));
  test_syscall(sem_up(sem));
  test_syscall(sem_close(sem));
  test_syscall(sem_close(sem));

  int all_open = 1;
  for (int i = 0; i < 100; ++i){
    sems[i] = sem_open(0);
    if (sems[i] < 100){
      all_open = 0;
    }
  }
  test_syscall(all_open);
  test_syscall(sem_open(0));

  for (int i = 0; i < 100; ++i){
    if (sems[i] >= 100){
      sem_close(sems[i]);
    }
  }

  sem = sem_open(0);
  test_syscall(sem >= 100);
  test_syscall(sem_close(sem));

  anon = mmap(sizeof(unsigned) * 2, MMAP_ANON, 0, MMAP_READ | MMAP_WRITE);
  test_syscall((int)anon != 0 && (int)anon != -1);
  anon[0] = 123;
  anon[1] = 456;
  test_syscall(anon[0]);
  test_syscall(anon[1]);

  int data_fd = open("data.txt");
  test_syscall(data_fd >= 0);
  file_map = mmap(4, data_fd, 0, MMAP_READ);
  test_syscall((int)file_map != 0 && (int)file_map != -1);
  test_syscall(file_map[0]);
  test_syscall(file_map[3]);
  test_syscall((int)mmap(4, 99, 0, MMAP_READ));
  test_syscall(close(data_fd));

  int audio_fd = open("test.wav");
  test_syscall(audio_fd >= 0);
  test_syscall(write(audio_fd, wav_bytes, TEST_WAV_BYTES));
  test_syscall(seek(audio_fd, 0, SEEK_SET));
  test_syscall(play_audio_file(STDOUT));
  test_syscall(play_audio_file(audio_fd));
  sleep(1);
  test_syscall(close(audio_fd));

  return 0;
}
