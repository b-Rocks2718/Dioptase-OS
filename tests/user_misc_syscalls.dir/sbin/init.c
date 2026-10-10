/*
 * user_misc_syscalls guest:
 * - validate the user-visible wrappers for jiffies, sleep, getkey, semaphore,
 *   mmap, request_priority, and the audio device
 * - ensure semaphore exhaustion returns -1 instead of dereferencing an invalid
 *   descriptor slot
 * - ensure incrementing an INT_MAX semaphore returns -1 without overflowing
 *   or changing its count
 * - ensure near-maximum CRT allocation/growth requests fail instead of
 *   wrapping to tiny successful allocations, and that failed realloc keeps
 *   the original allocation valid
 * - issue a raw trap and verify the trap ABI preserves the user's r29 return
 *   address even though the kernel wrapper makes nested C calls
 * - reject malformed mmap arguments, user-range exhaustion, and pipe or
 *   directory descriptors without reaching kernel VM/Node assertions
 * - reject seek on pipe endpoints before their Pipe storage can be mistaken
 *   for a filesystem Node, and make the user-space WAV parser (crt/wav.h)
 *   report a read failure for them rather than blocking or misparsing
 * - make the WAV parser name the exact reason for an empty file, truncated
 *   headers/chunks, unsupported channels/sample rate/sample width, empty
 *   data, and an odd PCM byte count, then accept a tiny valid WAV generated
 *   in place
 * - exercise the audio device ownership contract: operations without
 *   ownership fail with EBADF, a second open fails with EBUSY (also from a
 *   forked child), odd lengths fail with EINVAL, an unreadable buffer fails
 *   with EFAULT, writes accept at most the ring's free space, and a child
 *   that exits while owning the device releases it before wait_child()
 *   returns
 */

#include "../../../root/crt/sys.h"
#include "../../../root/crt/limits.h"
#include "../../../root/crt/stdlib.h"
#include "../../../root/crt/errno.h"
#include "../../../root/crt/string.h"
#include "../../../root/crt/wav.h"
#include "../../user_test.h"

#define TEST_WAV_BYTES 46
#define TEST_WAV_PCM_FORMAT 1U
#define TEST_WAV_CHANNELS 1U
#define TEST_WAV_SAMPLE_RATE 25000U
#define TEST_WAV_BYTE_RATE 50000U
#define TEST_WAV_BLOCK_ALIGN 2U
#define TEST_WAV_BITS_PER_SAMPLE 16U
#define TEST_WAV_DATA_BYTES 2U
#define TEST_WAV_RIFF_PREFIX_BYTES 8U
#define TEST_WAV_RIFF_SIZE_OFFSET 4U
#define TEST_WAV_CHANNELS_OFFSET 22U
#define TEST_WAV_SAMPLE_RATE_OFFSET 24U
#define TEST_WAV_BITS_PER_SAMPLE_OFFSET 34U
#define TEST_WAV_DATA_SIZE_OFFSET 40U
#define TEST_WAV_SHORT_HEADER_BYTES 11U
#define TEST_WAV_TRUNCATED_FMT_BYTES 24U
#define TEST_WAV_UNSUPPORTED_CHANNELS 2U
#define TEST_WAV_UNSUPPORTED_SAMPLE_RATE 44100U
#define TEST_WAV_UNSUPPORTED_BITS_PER_SAMPLE 8U

// Bit 1 is deliberately outside the documented sharing/protection flag set.
#define INVALID_MMAP_FLAG 0x02U

// The user half cannot encode an exclusive VME end above 0xFFFFF000. A
// reservation this large would consume its entire representable capacity and
// therefore cannot fit around the program and stacks already in this process.
#define FULL_USER_VME_CAPACITY 0x7FFFF000U

extern int raw_trap_preserves_ra(void);

static void write_u16_le(char* bytes, unsigned value){ /* Encode the low 16 bits in little-endian byte order. */
  bytes[0] = value & 0xFF;
  bytes[1] = (value >> 8) & 0xFF;
}

static void write_u32_le(char* bytes, unsigned value){ /* Encode a 32-bit value in little-endian byte order. */
  bytes[0] = value & 0xFF;
  bytes[1] = (value >> 8) & 0xFF;
  bytes[2] = (value >> 16) & 0xFF;
  bytes[3] = (value >> 24) & 0xFF;
}

static void fill_test_wav(char* wav_bytes){ /* Build the minimal valid PCM WAV fixture used by syscall validation cases. */
  wav_bytes[0] = 'R';
  wav_bytes[1] = 'I';
  wav_bytes[2] = 'F';
  wav_bytes[3] = 'F';
  write_u32_le(wav_bytes + TEST_WAV_RIFF_SIZE_OFFSET,
    TEST_WAV_BYTES - TEST_WAV_RIFF_PREFIX_BYTES);
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

int main(void){ /* Exercise miscellaneous user syscall validation paths. */
  int sems[100];
  int sem;
  char wav_bytes[TEST_WAV_BYTES];
  struct WavInfo wav_info;
  struct WavError wav_error;
  unsigned* anon;
  char* file_map;
  int pipe_fds[2];

  fill_test_wav(wav_bytes);

  user_test_expect_eq("malloc(UINT_MAX) fails safely",
    malloc(UINT_MAX) == (void*)0, 1);
  char* realloc_original = malloc(4);
  realloc_original[0] = 'R';
  user_test_expect_eq("realloc(UINT_MAX) fails safely",
    realloc(realloc_original, UINT_MAX) == (void*)0, 1);
  user_test_expect_eq("failed realloc preserves original allocation",
    realloc_original[0], 'R');
  free(realloc_original);
  user_test_expect_eq("raw trap preserves r29", raw_trap_preserves_ra(), 1);
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

  sem = sem_open(INT_MAX);
  user_test_expect_eq("open INT_MAX semaphore", sem >= 100, 1);
  user_test_expect_eq("sem_up rejects INT_MAX overflow", sem_up(sem), -1);
  user_test_expect_eq("failed sem_up preserves INT_MAX permit",
    sem_down(sem), 0);
  user_test_expect_eq("sem_up succeeds after one sem_down", sem_up(sem), 0);
  user_test_expect_eq("sem_close INT_MAX semaphore", sem_close(sem), 0);

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
  user_test_expect_eq("mmap rejects zero size",
    (int)mmap(0, MMAP_ANON, 0, MMAP_READ), -1);
  user_test_expect_eq("mmap rejects high-bit size",
    (int)mmap(UINT_MAX, MMAP_ANON, 0, MMAP_READ), -1);
  user_test_expect_eq("mmap rejects non-sentinel negative fd",
    (int)mmap(4, -2, 0, MMAP_READ), -1);
  user_test_expect_eq("mmap rejects anonymous offset",
    (int)mmap(4, MMAP_ANON, 4, MMAP_READ), -1);
  user_test_expect_eq("mmap rejects unknown flag",
    (int)mmap(4, MMAP_ANON, 0, INVALID_MMAP_FLAG), -1);
  user_test_expect_eq("mmap rejects shared anonymous mapping",
    (int)mmap(4, MMAP_ANON, 0, MMAP_READ | MMAP_SHARED), -1);
  user_test_expect_eq("mmap reports user range exhaustion",
    (int)mmap(FULL_USER_VME_CAPACITY, MMAP_ANON, 0, MMAP_READ), -1);

  int data_fd = open("data.txt");
  user_test_expect_eq("data_fd >= 0", data_fd >= 0, 1);
  file_map = mmap(4, data_fd, 0, MMAP_READ);
  user_test_expect_eq("(int)file_map != 0 && (int)file_map != -1", (int)file_map != 0 && (int)file_map != -1, 1);
  user_test_expect_eq("file mapping first byte", file_map[0], 77);
  user_test_expect_eq("file mapping fourth byte", file_map[3], 33);
  user_test_expect_eq("(int)mmap(4, 99, 0, MMAP_READ)", (int)mmap(4, 99, 0, MMAP_READ), -1);
  user_test_expect_eq("mmap rejects unaligned file offset",
    (int)mmap(4, data_fd, 1, MMAP_READ), -1);
  user_test_expect_eq("close(data_fd)", close(data_fd), 0);

  int directory_fd = open("/");
  user_test_expect_eq("directory_fd >= 0", directory_fd >= 0, 1);
  user_test_expect_eq("mmap rejects directory descriptor",
    (int)mmap(4, directory_fd, 0, MMAP_READ), -1);
  user_test_expect_eq("close(directory_fd)", close(directory_fd), 0);

  user_test_expect_eq("pipe(pipe_fds)", pipe(pipe_fds), 0);
  user_test_expect_eq("mmap rejects pipe read descriptor",
    (int)mmap(4, pipe_fds[0], 0, MMAP_READ), -1);
  user_test_expect_eq("mmap rejects pipe write descriptor",
    (int)mmap(4, pipe_fds[1], 0, MMAP_READ), -1);
  user_test_expect_eq("seek rejects pipe read descriptor",
    seek(pipe_fds[0], 0, SEEK_SET), -1);
  user_test_expect_eq("seek rejects pipe write descriptor",
    seek(pipe_fds[1], 0, SEEK_END), -1);
  user_test_expect_eq("wav header rejects pipe read descriptor",
    wav_read_header(pipe_fds[0], &wav_info, &wav_error), WAV_ERROR_READ);
  user_test_expect_eq("wav header rejects pipe write descriptor",
    wav_read_header(pipe_fds[1], &wav_info, &wav_error), WAV_ERROR_READ);
  user_test_expect_eq("close pipe read descriptor", close(pipe_fds[0]), 0);
  user_test_expect_eq("close pipe write descriptor", close(pipe_fds[1]), 0);

  int empty_audio_fd = open("empty.wav");
  user_test_expect_eq("empty_audio_fd >= 0", empty_audio_fd >= 0, 1);
  user_test_expect_eq("wav rejects empty regular file",
    wav_read_header(empty_audio_fd, &wav_info, &wav_error), WAV_ERROR_FILE_TOO_SMALL);
  user_test_expect_eq("close(empty_audio_fd)", close(empty_audio_fd), 0);

  fill_test_wav(wav_bytes);
  int short_audio_fd = open("short.wav");
  user_test_expect_eq("short_audio_fd >= 0", short_audio_fd >= 0, 1);
  user_test_expect_eq("write truncated RIFF header",
    write(short_audio_fd, wav_bytes, TEST_WAV_SHORT_HEADER_BYTES),
    TEST_WAV_SHORT_HEADER_BYTES);
  user_test_expect_eq("wav rejects truncated RIFF header",
    wav_read_header(short_audio_fd, &wav_info, &wav_error), WAV_ERROR_FILE_TOO_SMALL);
  user_test_expect_eq("close(short_audio_fd)", close(short_audio_fd), 0);

  // The RIFF size ends the file 4 bytes into the 16-byte fmt chunk.
  fill_test_wav(wav_bytes);
  write_u32_le(wav_bytes + TEST_WAV_RIFF_SIZE_OFFSET,
    TEST_WAV_TRUNCATED_FMT_BYTES - TEST_WAV_RIFF_PREFIX_BYTES);
  int truncated_fmt_fd = open("truncated_fmt.wav");
  user_test_expect_eq("truncated_fmt_fd >= 0", truncated_fmt_fd >= 0, 1);
  user_test_expect_eq("write truncated fmt chunk",
    write(truncated_fmt_fd, wav_bytes, TEST_WAV_TRUNCATED_FMT_BYTES),
    TEST_WAV_TRUNCATED_FMT_BYTES);
  user_test_expect_eq("wav rejects truncated fmt chunk",
    wav_read_header(truncated_fmt_fd, &wav_info, &wav_error), WAV_ERROR_CHUNK_PAST_RIFF);
  user_test_expect_eq("close(truncated_fmt_fd)", close(truncated_fmt_fd), 0);

  fill_test_wav(wav_bytes);
  write_u16_le(wav_bytes + TEST_WAV_CHANNELS_OFFSET,
    TEST_WAV_UNSUPPORTED_CHANNELS);
  int stereo_audio_fd = open("stereo.wav");
  user_test_expect_eq("stereo_audio_fd >= 0", stereo_audio_fd >= 0, 1);
  user_test_expect_eq("write unsupported stereo WAV",
    write(stereo_audio_fd, wav_bytes, TEST_WAV_BYTES), TEST_WAV_BYTES);
  user_test_expect_eq("wav rejects unsupported stereo WAV",
    wav_read_header(stereo_audio_fd, &wav_info, &wav_error), WAV_ERROR_CHANNELS);
  user_test_expect_eq("close(stereo_audio_fd)", close(stereo_audio_fd), 0);

  fill_test_wav(wav_bytes);
  write_u32_le(wav_bytes + TEST_WAV_SAMPLE_RATE_OFFSET,
    TEST_WAV_UNSUPPORTED_SAMPLE_RATE);
  int sample_rate_fd = open("sample_rate.wav");
  user_test_expect_eq("sample_rate_fd >= 0", sample_rate_fd >= 0, 1);
  user_test_expect_eq("write unsupported sample-rate WAV",
    write(sample_rate_fd, wav_bytes, TEST_WAV_BYTES), TEST_WAV_BYTES);
  user_test_expect_eq("wav rejects unsupported sample-rate WAV",
    wav_read_header(sample_rate_fd, &wav_info, &wav_error), WAV_ERROR_SAMPLE_RATE);
  user_test_expect_eq("close(sample_rate_fd)", close(sample_rate_fd), 0);

  fill_test_wav(wav_bytes);
  write_u16_le(wav_bytes + TEST_WAV_BITS_PER_SAMPLE_OFFSET,
    TEST_WAV_UNSUPPORTED_BITS_PER_SAMPLE);
  int sample_width_fd = open("sample_width.wav");
  user_test_expect_eq("sample_width_fd >= 0", sample_width_fd >= 0, 1);
  user_test_expect_eq("write unsupported sample-width WAV",
    write(sample_width_fd, wav_bytes, TEST_WAV_BYTES), TEST_WAV_BYTES);
  user_test_expect_eq("wav rejects unsupported sample-width WAV",
    wav_read_header(sample_width_fd, &wav_info, &wav_error), WAV_ERROR_BITS_PER_SAMPLE);
  user_test_expect_eq("close(sample_width_fd)", close(sample_width_fd), 0);

  fill_test_wav(wav_bytes);
  write_u32_le(wav_bytes + TEST_WAV_DATA_SIZE_OFFSET, 0);
  int empty_data_fd = open("empty_data.wav");
  user_test_expect_eq("empty_data_fd >= 0", empty_data_fd >= 0, 1);
  user_test_expect_eq("write zero-data WAV",
    write(empty_data_fd, wav_bytes, TEST_WAV_BYTES), TEST_WAV_BYTES);
  user_test_expect_eq("wav rejects zero-data WAV",
    wav_read_header(empty_data_fd, &wav_info, &wav_error), WAV_ERROR_DATA_EMPTY);
  user_test_expect_eq("close(empty_data_fd)", close(empty_data_fd), 0);

  fill_test_wav(wav_bytes);
  write_u32_le(wav_bytes + TEST_WAV_DATA_SIZE_OFFSET, 1);
  int odd_data_fd = open("odd_data.wav");
  user_test_expect_eq("odd_data_fd >= 0", odd_data_fd >= 0, 1);
  user_test_expect_eq("write odd-data WAV",
    write(odd_data_fd, wav_bytes, TEST_WAV_BYTES), TEST_WAV_BYTES);
  user_test_expect_eq("wav rejects odd-data WAV",
    wav_read_header(odd_data_fd, &wav_info, &wav_error), WAV_ERROR_DATA_MISALIGNED);
  user_test_expect_eq("close(odd_data_fd)", close(odd_data_fd), 0);

  fill_test_wav(wav_bytes);
  int audio_fd = open("test.wav");
  user_test_expect_eq("audio_fd >= 0", audio_fd >= 0, 1);
  user_test_expect_eq("write(audio_fd, wav_bytes, TEST_WAV_BYTES)", write(audio_fd, wav_bytes, TEST_WAV_BYTES), 46);
  user_test_expect_eq("wav accepts valid header",
    wav_read_header(audio_fd, &wav_info, &wav_error), WAV_OK);
  user_test_expect_eq("valid wav data offset", wav_info.data_offset, 44);
  user_test_expect_eq("valid wav data size", wav_info.data_size, TEST_WAV_DATA_BYTES);
  user_test_expect_eq("close(audio_fd)", close(audio_fd), 0);

  // Without ownership every operation is refused and the device is untouched.
  char pcm[8];
  memset(pcm, 0, sizeof(pcm));
  errno = 0;
  user_test_expect_eq("audio_write without ownership", audio_write(pcm, 8), -1);
  user_test_expect_eq("audio_write without ownership errno", errno, EBADF);
  errno = 0;
  user_test_expect_eq("audio_buffered without ownership", audio_buffered(), -1);
  user_test_expect_eq("audio_buffered without ownership errno", errno, EBADF);
  errno = 0;
  user_test_expect_eq("audio_close without ownership", audio_close(), -1);
  user_test_expect_eq("audio_close without ownership errno", errno, EBADF);

  user_test_expect_eq("audio_open", audio_open(), 0);
  errno = 0;
  user_test_expect_eq("second audio_open", audio_open(), -1);
  user_test_expect_eq("second audio_open errno", errno, EBUSY);

  // A different process cannot take the device while this one owns it.
  int busy_child = fork();
  if (busy_child == 0){
    errno = 0;
    int rc = audio_open();
    return rc == -1 && errno == EBUSY ? 1 : 0;
  }
  user_test_expect_eq("child audio_open sees EBUSY", wait_child(busy_child), 1);

  errno = 0;
  user_test_expect_eq("audio_write odd length", audio_write(pcm, 3), -1);
  user_test_expect_eq("audio_write odd length errno", errno, EINVAL);
  // Address 4 is below the user half, so the accepted prefix is unreadable.
  errno = 0;
  user_test_expect_eq("audio_write unreadable buffer", audio_write((void*)4, 8), -1);
  user_test_expect_eq("audio_write unreadable buffer errno", errno, EFAULT);

  user_test_expect_eq("audio_write 4 samples", audio_write(pcm, 8), 8);
  int buffered = audio_buffered();
  user_test_expect_eq("audio_buffered within written bytes", buffered >= 0 && buffered <= 8, 1);

  // The ring holds 16382 bytes, so a 20000-byte write is only partly accepted:
  // exactly the free space at the moment of the write. Playback keeps
  // draining the 8 bytes above while this code runs, so the fill at write time
  // is only known to lie in [0, buffered] (this process is the only producer).
  // The accepted count must therefore lie in [16382 - buffered, 16382].
  // Comparing against 16382 - buffered alone would race with the device: a
  // sample played after audio_buffered() legitimately grows the free space.
  char* silence = malloc(20000);
  memset(silence, 0, 20000);
  int accepted = audio_write(silence, 20000);
  user_test_expect_eq("large audio_write accepted part", accepted > 0 && accepted < 20000, 1);
  user_test_expect_eq("large audio_write fits the ring",
    accepted >= 16382 - buffered && accepted <= 16382, 1);
  free(silence);

  user_test_expect_eq("audio_close", audio_close(), 0);
  errno = 0;
  user_test_expect_eq("audio_close twice", audio_close(), -1);
  user_test_expect_eq("audio_close twice errno", errno, EBADF);

  // A child that exits while owning the device must release it before its
  // exit becomes visible to wait_child().
  int owner_child = fork();
  if (owner_child == 0){
    if (audio_open() != 0 || audio_write(pcm, 8) != 8){
      return 0;
    }
    return 1;
  }
  user_test_expect_eq("owning child played", wait_child(owner_child), 1);
  user_test_expect_eq("audio_open after owner exit", audio_open(), 0);
  user_test_expect_eq("audio_close after reopen", audio_close(), 0);

  return 0;
}
