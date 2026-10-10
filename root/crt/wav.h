#ifndef WAV_H
#define WAV_H

#include "stdbool.h"

/*
 * WAV playback on the audio device (sys.h audio_* syscalls).
 *
 * The device accepts exactly one format: signed 16-bit little-endian mono PCM
 * at 25,000 samples/second (docs/mem_map.md). wav_read_header() validates that
 * a file's RIFF structure and fmt chunk match it; a WavStream then feeds the
 * data chunk to the device in small pieces so programs never block on audio:
 *
 *   struct WavStream stream;
 *   struct WavError error;
 *   if (wav_stream_open(&stream, "/song.wav", &error) != WAV_OK) { ... }
 *   while (!wav_stream_finished(&stream)){
 *     if (wav_stream_pump(&stream) < 0) { ... }
 *     ... other work for at most ~0.3 s (the device ring's capacity) ...
 *   }
 *   wav_stream_close(&stream);
 */

#define WAV_SAMPLE_RATE 25000
#define WAV_SAMPLE_BYTES 2

// Why a WAV file was rejected or a stream could not start.
enum WavErrorCode {
  WAV_OK = 0,
  WAV_ERROR_OPEN,                   // the file could not be opened
  WAV_ERROR_READ,                   // a read or seek failed or came up short
  WAV_ERROR_DEVICE_BUSY,            // another process owns the audio device
  WAV_ERROR_FILE_TOO_SMALL,
  WAV_ERROR_RIFF_SIGNATURE,
  WAV_ERROR_WAVE_SIGNATURE,
  WAV_ERROR_RIFF_SIZE_TOO_SMALL,
  WAV_ERROR_RIFF_SIZE_PAST_FILE,
  WAV_ERROR_CHUNK_HEADER_TRUNCATED,
  WAV_ERROR_CHUNK_SIZE_OVERFLOW,
  WAV_ERROR_CHUNK_PAST_RIFF,
  WAV_ERROR_CHUNK_PADDING_PAST_RIFF,
  WAV_ERROR_FMT_TOO_SHORT,
  WAV_ERROR_AUDIO_FORMAT,
  WAV_ERROR_CHANNELS,
  WAV_ERROR_SAMPLE_RATE,
  WAV_ERROR_BYTE_RATE,
  WAV_ERROR_BLOCK_ALIGN,
  WAV_ERROR_BITS_PER_SAMPLE,
  WAV_ERROR_DATA_BEFORE_FMT,
  WAV_ERROR_DATA_EMPTY,
  WAV_ERROR_DATA_MISALIGNED,
  WAV_ERROR_DATA_MISSING,
};

// A rejection with the file offset of the offending field and its value.
struct WavError {
  enum WavErrorCode code;
  unsigned offset;
  unsigned got;
  unsigned expected;
};

// Location of the validated PCM payload inside the file.
struct WavInfo {
  unsigned file_size;
  unsigned data_offset;
  unsigned data_size;
};

// Short identifier for an error code, for diagnostics.
char* wav_error_name(enum WavErrorCode code);

// Print "<prefix>: <path>: <error> (offset, got, expected)" to stdout.
void wav_print_error(char* prefix, char* path, struct WavError* error);

// Validate the WAV in open file `fd` against the device format. On WAV_OK,
// `info` locates the PCM data; the file offset is left unspecified.
enum WavErrorCode wav_read_header(int fd, struct WavInfo* info, struct WavError* error);

// File data is read in pieces of this size between device writes. Kept in
// words so the buffer is 4-byte aligned; bcc array bounds must be plain
// constants, hence the separate word count.
#define WAV_STREAM_BUFFER_WORDS 1024
#define WAV_STREAM_BUFFER_BYTES (WAV_STREAM_BUFFER_WORDS * 4)

// A WAV file being played on the audio device. Fields are private to wav.c.
struct WavStream {
  int fd;
  unsigned data_remaining;   // data-chunk bytes not yet read from the file
  unsigned pending_offset;   // next buffered byte not yet accepted by the device
  unsigned pending_end;      // number of valid bytes in buffer
  unsigned accepted;         // total bytes the device has accepted
  unsigned buffer[WAV_STREAM_BUFFER_WORDS];
};

// Open and validate `path`, claim the audio device, and queue the first
// audio. Returns WAV_OK, or an error with the device and file left released.
enum WavErrorCode wav_stream_open(struct WavStream* stream, char* path, struct WavError* error);

// Queue as much more audio as the device accepts right now. Never waits.
// Returns 0, or -1 if the file or device failed (playback then stops).
int wav_stream_pump(struct WavStream* stream);

// Number of samples the device has played so far. Divide by WAV_SAMPLE_RATE
// for seconds; this is the playback clock to synchronize visuals with.
unsigned wav_stream_position(struct WavStream* stream);

// Whether every sample of the file has been played.
bool wav_stream_finished(struct WavStream* stream);

// Stop playback (discarding anything still queued), release the device, and
// close the file.
void wav_stream_close(struct WavStream* stream);

#endif // WAV_H
