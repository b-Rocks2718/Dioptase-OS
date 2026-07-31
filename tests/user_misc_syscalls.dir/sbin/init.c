/*
 * user_misc_syscalls guest:
 * - validate the user-visible wrappers for jiffies, sleep, getkey, semaphore,
 *   mmap, request_priority, and play_audio_file
 * - ensure semaphore exhaustion returns -1 instead of dereferencing an invalid
 *   descriptor slot
 * - generate a tiny valid WAV file in-place so play_audio_file can take a real
 *   success path without needing a checked-in binary fixture
 */

#include "../../../root/crt/sys.h"
#include "../../user_test.h"

#define TEST_WAV_BYTES 46
#define TEST_WAV_PCM_FORMAT 1U
#define TEST_WAV_CHANNELS 1U
#define TEST_WAV_SAMPLE_RATE 25000U
#define TEST_WAV_BYTE_RATE 50000U
#define TEST_WAV_BLOCK_ALIGN 2U
#define TEST_WAV_BITS_PER_SAMPLE 16U
#define TEST_WAV_DATA_BYTES 2U

static void write_u16_le(char* bytes, unsigned value){
  bytes[0] = value & 0xFF;
  bytes[1] = (value >> 8) & 0xFF;
}

static void write_u32_le(char* bytes, unsigned value){
  bytes[0] = value & 0xFF;
  bytes[1] = (value >> 8) & 0xFF;
  bytes[2] = (value >> 16) & 0xFF;
  bytes[3] = (value >> 24) & 0xFF;
}

static void fill_test_wav(char* wav_bytes){
  wav_bytes[0] = 'R';
  wav_bytes[1] = 'I';
  wav_bytes[2] = 'F';
  wav_bytes[3] = 'F';
  write_u32_le(wav_bytes + 4, TEST_WAV_BYTES - 8);
  wav_bytes[8] = 'W';
  wav_bytes[9] = 'A';
  wav_bytes[10] = 'V';
  wav_bytes[11] = 'E';
  wav_bytes[12] = 'f';
  wav_bytes[13] = 'm';
  wav_bytes[14] = 't';
  wav_bytes[15] = ' ';
  write_u32_le(wav_bytes + 16, 16);
  write_u16_le(wav_bytes + 20, TEST_WAV_PCM_FORMAT);
  write_u16_le(wav_bytes + 22, TEST_WAV_CHANNELS);
  write_u32_le(wav_bytes + 24, TEST_WAV_SAMPLE_RATE);
  write_u32_le(wav_bytes + 28, TEST_WAV_BYTE_RATE);
  write_u16_le(wav_bytes + 32, TEST_WAV_BLOCK_ALIGN);
  write_u16_le(wav_bytes + 34, TEST_WAV_BITS_PER_SAMPLE);
  wav_bytes[36] = 'd';
  wav_bytes[37] = 'a';
  wav_bytes[38] = 't';
  wav_bytes[39] = 'a';
  write_u32_le(wav_bytes + 40, TEST_WAV_DATA_BYTES);
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

  user_test_expect_eq("getkey()", getkey(), 0);

  unsigned start = get_current_jiffies();
  sleep(3);
  user_test_expect_eq("get_current_jiffies() - start >= 3", get_current_jiffies() - start >= 3, 1);

  yield();
  user_test_expect_eq("yield resumed the current process", 1, 1);

  user_test_expect_eq("request_priority(-1)", request_priority(-1), -1);
  user_test_expect_eq("request_priority(DIOPTASE_PRIORITY_LOW)", request_priority(DIOPTASE_PRIORITY_LOW), 0);
  user_test_expect_eq("request_priority(DIOPTASE_PRIORITY_HIGH)", request_priority(DIOPTASE_PRIORITY_HIGH), 0);
  user_test_expect_eq("request_priority(DIOPTASE_PRIORITY_HIGH + 1)", request_priority(DIOPTASE_PRIORITY_HIGH + 1), -1);
  user_test_expect_eq("request_priority(DIOPTASE_PRIORITY_NORMAL)", request_priority(DIOPTASE_PRIORITY_NORMAL), 0);

  user_test_expect_eq("sem_open(-1)", sem_open(-1), -1);

  sem = sem_open(1);
  user_test_expect_eq("open semaphore descriptor", sem >= 100, 1);
  user_test_expect_eq("sem_down(sem)", sem_down(sem), 0);
  user_test_expect_eq("sem_up(sem)", sem_up(sem), 0);
  user_test_expect_eq("sem_close(sem)", sem_close(sem), 0);
  user_test_expect_eq("sem_close(sem)", sem_close(sem), -1);

  int all_open = 1;
  for (int i = 0; i < 100; ++i){
    sems[i] = sem_open(0);
    if (sems[i] < 100){
      all_open = 0;
    }
  }
  user_test_expect_eq("all_open", all_open, 1);
  user_test_expect_eq("sem_open(0)", sem_open(0), -1);

  for (int i = 0; i < 100; ++i){
    if (sems[i] >= 100){
      sem_close(sems[i]);
    }
  }

  sem = sem_open(0);
  user_test_expect_eq("reuse semaphore descriptor after exhaustion",
    sem >= 100, 1);
  user_test_expect_eq("sem_close(sem)", sem_close(sem), 0);

  anon = mmap(sizeof(unsigned) * 2, MMAP_ANON, 0, MMAP_READ | MMAP_WRITE);
  user_test_expect_eq("(int)anon != 0 && (int)anon != -1", (int)anon != 0 && (int)anon != -1, 1);
  anon[0] = 123;
  anon[1] = 456;
  user_test_expect_eq("anonymous mapping first word", anon[0], 123);
  user_test_expect_eq("anonymous mapping second word", anon[1], 456);

  int data_fd = open("data.txt");
  user_test_expect_eq("data_fd >= 0", data_fd >= 0, 1);
  file_map = mmap(4, data_fd, 0, MMAP_READ);
  user_test_expect_eq("(int)file_map != 0 && (int)file_map != -1", (int)file_map != 0 && (int)file_map != -1, 1);
  user_test_expect_eq("file mapping first byte", file_map[0], 77);
  user_test_expect_eq("file mapping fourth byte", file_map[3], 33);
  user_test_expect_eq("(int)mmap(4, 99, 0, MMAP_READ)", (int)mmap(4, 99, 0, MMAP_READ), -1);
  user_test_expect_eq("close(data_fd)", close(data_fd), 0);

  int audio_fd = open("test.wav");
  user_test_expect_eq("audio_fd >= 0", audio_fd >= 0, 1);
  user_test_expect_eq("write(audio_fd, wav_bytes, TEST_WAV_BYTES)", write(audio_fd, wav_bytes, TEST_WAV_BYTES), 46);
  user_test_expect_eq("seek(audio_fd, 0, SEEK_SET)", seek(audio_fd, 0, SEEK_SET), 0);
  user_test_expect_eq("play_audio_file(STDOUT)", play_audio_file(STDOUT), -1);
  user_test_expect_eq("play_audio_file(audio_fd)", play_audio_file(audio_fd), 0);
  sleep(1);
  user_test_expect_eq("close(audio_fd)", close(audio_fd), 0);

  return 0;
}
