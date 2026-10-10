#include "wav.h"

#include "errno.h"
#include "limits.h"
#include "print.h"
#include "sys.h"
#include "unistd.h"

/*
 * WAV validation and streaming playback in user mode.
 *
 * The parser reads only the RIFF header, chunk headers, and the fmt chunk, and
 * checks every size against both the file size and the declared RIFF extent
 * before using it, so a malformed file is rejected with a precise reason
 * rather than misread. The checks match the device format in docs/mem_map.md.
 */

#define WAV_RIFF_HEADER_BYTES 12
#define WAV_RIFF_SIZE_OFFSET 4
#define WAV_WAVE_ID_OFFSET 8
#define WAV_RIFF_SIZE_PREFIX_BYTES 8
#define WAV_FORM_TYPE_BYTES 4
#define WAV_CHUNK_HEADER_BYTES 8
#define WAV_CHUNK_SIZE_OFFSET 4
#define WAV_FMT_MIN_BYTES 16
#define WAV_FMT_AUDIO_FORMAT_OFFSET 0
#define WAV_FMT_CHANNELS_OFFSET 2
#define WAV_FMT_SAMPLE_RATE_OFFSET 4
#define WAV_FMT_BYTE_RATE_OFFSET 8
#define WAV_FMT_BLOCK_ALIGN_OFFSET 12
#define WAV_FMT_BITS_PER_SAMPLE_OFFSET 14
#define WAV_PCM_FORMAT 1
#define WAV_EXPECTED_CHANNELS 1
#define WAV_EXPECTED_BITS_PER_SAMPLE 16
#define WAV_EXPECTED_BLOCK_ALIGN 2
#define WAV_EXPECTED_BYTE_RATE (WAV_SAMPLE_RATE * WAV_SAMPLE_BYTES)

// "RIFF" and "WAVE" read as little-endian words, reported on mismatch.
#define WAV_RIFF_ID_LE 0x46464952
#define WAV_WAVE_ID_LE 0x45564157

// Decode a little-endian 16-bit field.
static unsigned read_u16_le(char* bytes){
  return ((unsigned)(unsigned char)bytes[0]) |
         ((unsigned)(unsigned char)bytes[1] << 8);
}

// Decode a little-endian 32-bit field.
static unsigned read_u32_le(char* bytes){
  return ((unsigned)(unsigned char)bytes[0]) |
         ((unsigned)(unsigned char)bytes[1] << 8) |
         ((unsigned)(unsigned char)bytes[2] << 16) |
         ((unsigned)(unsigned char)bytes[3] << 24);
}

// Return whether four bytes match a chunk identifier.
static bool chunk_id_is(char* bytes, char a, char b, char c, char d){
  return bytes[0] == a && bytes[1] == b && bytes[2] == c && bytes[3] == d;
}

// Record a rejection and return its code.
static enum WavErrorCode wav_reject(struct WavError* error, enum WavErrorCode code,
    unsigned offset, unsigned got, unsigned expected){
  error->code = code;
  error->offset = offset;
  error->got = got;
  error->expected = expected;
  return code;
}

// Read exactly `count` bytes at `offset`; false on a seek/read failure or EOF.
static bool read_at(int fd, unsigned offset, char* buf, unsigned count){
  if (seek(fd, offset, SEEK_SET) != (int)offset){
    return false;
  }
  unsigned done = 0;
  while (done < count){
    int n = read(fd, buf + done, count - done);
    if (n <= 0){
      return false;
    }
    done += n;
  }
  return true;
}

// Map an error code to a short diagnostic name.
char* wav_error_name(enum WavErrorCode code){
  switch (code){
    case WAV_OK: return "ok";
    case WAV_ERROR_OPEN: return "open_failed";
    case WAV_ERROR_READ: return "read_failed";
    case WAV_ERROR_DEVICE_BUSY: return "audio_device_busy";
    case WAV_ERROR_FILE_TOO_SMALL: return "file_too_small";
    case WAV_ERROR_RIFF_SIGNATURE: return "riff_signature";
    case WAV_ERROR_WAVE_SIGNATURE: return "wave_signature";
    case WAV_ERROR_RIFF_SIZE_TOO_SMALL: return "riff_size_too_small";
    case WAV_ERROR_RIFF_SIZE_PAST_FILE: return "riff_size_past_file";
    case WAV_ERROR_CHUNK_HEADER_TRUNCATED: return "chunk_header_truncated";
    case WAV_ERROR_CHUNK_SIZE_OVERFLOW: return "chunk_size_overflow";
    case WAV_ERROR_CHUNK_PAST_RIFF: return "chunk_past_riff";
    case WAV_ERROR_CHUNK_PADDING_PAST_RIFF: return "chunk_padding_past_riff";
    case WAV_ERROR_FMT_TOO_SHORT: return "fmt_too_short";
    case WAV_ERROR_AUDIO_FORMAT: return "unsupported_audio_format";
    case WAV_ERROR_CHANNELS: return "unsupported_channels";
    case WAV_ERROR_SAMPLE_RATE: return "unsupported_sample_rate";
    case WAV_ERROR_BYTE_RATE: return "unsupported_byte_rate";
    case WAV_ERROR_BLOCK_ALIGN: return "unsupported_block_align";
    case WAV_ERROR_BITS_PER_SAMPLE: return "unsupported_bits_per_sample";
    case WAV_ERROR_DATA_BEFORE_FMT: return "data_before_fmt";
    case WAV_ERROR_DATA_EMPTY: return "data_empty";
    case WAV_ERROR_DATA_MISALIGNED: return "data_misaligned";
    case WAV_ERROR_DATA_MISSING: return "data_missing";
  }
  return "unknown";
}

// Print one rejection with enough context to find the bad field.
void wav_print_error(char* prefix, char* path, struct WavError* error){
  void* args[6];
  args[0] = prefix;
  args[1] = path;
  args[2] = wav_error_name(error->code);
  args[3] = (void*)error->offset;
  args[4] = (void*)error->got;
  args[5] = (void*)error->expected;
  printf("%s: %s: %s (offset 0x%X, got 0x%X, expected 0x%X)\n", args);
}

// Validate the RIFF/WAVE structure of `fd` and locate its PCM data chunk.
enum WavErrorCode wav_read_header(int fd, struct WavInfo* info, struct WavError* error){
  char header[WAV_RIFF_HEADER_BYTES];
  char chunk[WAV_CHUNK_HEADER_BYTES];
  char fmt[WAV_FMT_MIN_BYTES];

  error->code = WAV_OK;
  error->offset = 0;
  error->got = 0;
  error->expected = 0;

  int end = seek(fd, 0, SEEK_END);
  if (end < 0){
    return wav_reject(error, WAV_ERROR_READ, 0, 0, 0);
  }
  unsigned file_size = end;
  if (file_size < WAV_RIFF_HEADER_BYTES){
    return wav_reject(error, WAV_ERROR_FILE_TOO_SMALL, 0, file_size, WAV_RIFF_HEADER_BYTES);
  }
  if (!read_at(fd, 0, header, WAV_RIFF_HEADER_BYTES)){
    return wav_reject(error, WAV_ERROR_READ, 0, 0, WAV_RIFF_HEADER_BYTES);
  }
  if (!chunk_id_is(header, 'R', 'I', 'F', 'F')){
    return wav_reject(error, WAV_ERROR_RIFF_SIGNATURE, 0, read_u32_le(header), WAV_RIFF_ID_LE);
  }
  if (!chunk_id_is(header + WAV_WAVE_ID_OFFSET, 'W', 'A', 'V', 'E')){
    return wav_reject(error, WAV_ERROR_WAVE_SIGNATURE, WAV_WAVE_ID_OFFSET,
      read_u32_le(header + WAV_WAVE_ID_OFFSET), WAV_WAVE_ID_LE);
  }

  unsigned riff_size = read_u32_le(header + WAV_RIFF_SIZE_OFFSET);
  if (riff_size < WAV_FORM_TYPE_BYTES){
    return wav_reject(error, WAV_ERROR_RIFF_SIZE_TOO_SMALL, WAV_RIFF_SIZE_OFFSET,
      riff_size, WAV_FORM_TYPE_BYTES);
  }
  if (riff_size > file_size - WAV_RIFF_SIZE_PREFIX_BYTES){
    return wav_reject(error, WAV_ERROR_RIFF_SIZE_PAST_FILE, WAV_RIFF_SIZE_OFFSET,
      riff_size, file_size - WAV_RIFF_SIZE_PREFIX_BYTES);
  }
  // The check above proves this addition cannot overflow.
  unsigned riff_end = WAV_RIFF_SIZE_PREFIX_BYTES + riff_size;

  bool saw_fmt = false;
  unsigned offset = WAV_RIFF_HEADER_BYTES;
  while (offset < riff_end){
    unsigned header_room = riff_end - offset;
    if (header_room < WAV_CHUNK_HEADER_BYTES){
      return wav_reject(error, WAV_ERROR_CHUNK_HEADER_TRUNCATED, offset,
        header_room, WAV_CHUNK_HEADER_BYTES);
    }
    if (!read_at(fd, offset, chunk, WAV_CHUNK_HEADER_BYTES)){
      return wav_reject(error, WAV_ERROR_READ, offset, 0, WAV_CHUNK_HEADER_BYTES);
    }

    unsigned chunk_size = read_u32_le(chunk + WAV_CHUNK_SIZE_OFFSET);
    unsigned data_offset = offset + WAV_CHUNK_HEADER_BYTES;
    unsigned capacity = riff_end - data_offset;

    // RIFF pads odd-sized chunks to an even length; check the pad addition.
    if ((chunk_size & 1) != 0 && chunk_size == UINT_MAX){
      return wav_reject(error, WAV_ERROR_CHUNK_SIZE_OVERFLOW,
        offset + WAV_CHUNK_SIZE_OFFSET, chunk_size, UINT_MAX - 1);
    }
    unsigned padded_size = chunk_size + (chunk_size & 1);
    if (chunk_size > capacity){
      return wav_reject(error, WAV_ERROR_CHUNK_PAST_RIFF,
        offset + WAV_CHUNK_SIZE_OFFSET, chunk_size, capacity);
    }
    if (padded_size > capacity){
      return wav_reject(error, WAV_ERROR_CHUNK_PADDING_PAST_RIFF,
        offset + WAV_CHUNK_SIZE_OFFSET, padded_size, capacity);
    }

    if (chunk_id_is(chunk, 'f', 'm', 't', ' ')){
      if (chunk_size < WAV_FMT_MIN_BYTES){
        return wav_reject(error, WAV_ERROR_FMT_TOO_SHORT,
          offset + WAV_CHUNK_SIZE_OFFSET, chunk_size, WAV_FMT_MIN_BYTES);
      }
      if (!read_at(fd, data_offset, fmt, WAV_FMT_MIN_BYTES)){
        return wav_reject(error, WAV_ERROR_READ, data_offset, 0, WAV_FMT_MIN_BYTES);
      }

      unsigned audio_format = read_u16_le(fmt + WAV_FMT_AUDIO_FORMAT_OFFSET);
      unsigned channels = read_u16_le(fmt + WAV_FMT_CHANNELS_OFFSET);
      unsigned sample_rate = read_u32_le(fmt + WAV_FMT_SAMPLE_RATE_OFFSET);
      unsigned byte_rate = read_u32_le(fmt + WAV_FMT_BYTE_RATE_OFFSET);
      unsigned block_align = read_u16_le(fmt + WAV_FMT_BLOCK_ALIGN_OFFSET);
      unsigned bits_per_sample = read_u16_le(fmt + WAV_FMT_BITS_PER_SAMPLE_OFFSET);

      if (audio_format != WAV_PCM_FORMAT){
        return wav_reject(error, WAV_ERROR_AUDIO_FORMAT,
          data_offset + WAV_FMT_AUDIO_FORMAT_OFFSET, audio_format, WAV_PCM_FORMAT);
      }
      if (channels != WAV_EXPECTED_CHANNELS){
        return wav_reject(error, WAV_ERROR_CHANNELS,
          data_offset + WAV_FMT_CHANNELS_OFFSET, channels, WAV_EXPECTED_CHANNELS);
      }
      if (sample_rate != WAV_SAMPLE_RATE){
        return wav_reject(error, WAV_ERROR_SAMPLE_RATE,
          data_offset + WAV_FMT_SAMPLE_RATE_OFFSET, sample_rate, WAV_SAMPLE_RATE);
      }
      if (bits_per_sample != WAV_EXPECTED_BITS_PER_SAMPLE){
        return wav_reject(error, WAV_ERROR_BITS_PER_SAMPLE,
          data_offset + WAV_FMT_BITS_PER_SAMPLE_OFFSET, bits_per_sample,
          WAV_EXPECTED_BITS_PER_SAMPLE);
      }
      if (block_align != WAV_EXPECTED_BLOCK_ALIGN){
        return wav_reject(error, WAV_ERROR_BLOCK_ALIGN,
          data_offset + WAV_FMT_BLOCK_ALIGN_OFFSET, block_align, WAV_EXPECTED_BLOCK_ALIGN);
      }
      if (byte_rate != WAV_EXPECTED_BYTE_RATE){
        return wav_reject(error, WAV_ERROR_BYTE_RATE,
          data_offset + WAV_FMT_BYTE_RATE_OFFSET, byte_rate, WAV_EXPECTED_BYTE_RATE);
      }
      saw_fmt = true;
    } else if (chunk_id_is(chunk, 'd', 'a', 't', 'a')){
      if (!saw_fmt){
        return wav_reject(error, WAV_ERROR_DATA_BEFORE_FMT, offset, 0, 1);
      }
      if (chunk_size == 0){
        return wav_reject(error, WAV_ERROR_DATA_EMPTY,
          offset + WAV_CHUNK_SIZE_OFFSET, 0, WAV_SAMPLE_BYTES);
      }
      if ((chunk_size % WAV_EXPECTED_BLOCK_ALIGN) != 0 ||
          (data_offset % WAV_EXPECTED_BLOCK_ALIGN) != 0){
        return wav_reject(error, WAV_ERROR_DATA_MISALIGNED, data_offset,
          chunk_size, WAV_EXPECTED_BLOCK_ALIGN);
      }
      info->file_size = file_size;
      info->data_offset = data_offset;
      info->data_size = chunk_size;
      return WAV_OK;
    }

    // padded_size <= capacity proves this addition cannot overflow.
    offset = data_offset + padded_size;
  }

  return wav_reject(error, WAV_ERROR_DATA_MISSING, riff_end, 0, 1);
}

// Open, validate, claim the device, and start playback.
enum WavErrorCode wav_stream_open(struct WavStream* stream, char* path, struct WavError* error){
  struct WavInfo info;

  error->code = WAV_OK;
  error->offset = 0;
  error->got = 0;
  error->expected = 0;

  stream->fd = open_existing(path);
  if (stream->fd < 0){
    return wav_reject(error, WAV_ERROR_OPEN, 0, 0, 0);
  }
  enum WavErrorCode code = wav_read_header(stream->fd, &info, error);
  if (code == WAV_OK && seek(stream->fd, info.data_offset, SEEK_SET) != (int)info.data_offset){
    code = wav_reject(error, WAV_ERROR_READ, info.data_offset, 0, 0);
  }
  if (code == WAV_OK && audio_open() < 0){
    code = wav_reject(error, WAV_ERROR_DEVICE_BUSY, 0, errno, 0);
  }
  if (code != WAV_OK){
    close(stream->fd);
    stream->fd = -1;
    return code;
  }

  stream->data_remaining = info.data_size;
  stream->pending_offset = 0;
  stream->pending_end = 0;
  stream->accepted = 0;

  if (wav_stream_pump(stream) < 0){
    wav_stream_close(stream);
    return wav_reject(error, WAV_ERROR_READ, info.data_offset, 0, 0);
  }
  return WAV_OK;
}

// Refill the staging buffer from the file and hand the device what fits.
int wav_stream_pump(struct WavStream* stream){
  char* buffer = (char*)stream->buffer;

  while (true){
    if (stream->pending_offset == stream->pending_end){
      if (stream->data_remaining == 0){
        return 0;
      }
      unsigned want = stream->data_remaining < WAV_STREAM_BUFFER_BYTES
        ? stream->data_remaining : WAV_STREAM_BUFFER_BYTES;
      unsigned got = 0;
      while (got < want){
        int n = read(stream->fd, buffer + got, want - got);
        if (n <= 0){
          // The header promised these bytes; the file shrank or failed.
          return -1;
        }
        got += n;
      }
      stream->pending_offset = 0;
      stream->pending_end = got;
      stream->data_remaining -= got;
    }

    int accepted = audio_write(buffer + stream->pending_offset,
      stream->pending_end - stream->pending_offset);
    if (accepted < 0){
      return -1;
    }
    stream->pending_offset += accepted;
    stream->accepted += accepted;
    if (stream->pending_offset < stream->pending_end){
      // The ring is full; the rest waits for the next pump.
      return 0;
    }
  }
}

// Samples played = samples accepted minus samples still queued in the ring.
unsigned wav_stream_position(struct WavStream* stream){
  int buffered = audio_buffered();
  if (buffered < 0){
    buffered = 0;
  }
  return (stream->accepted - buffered) / WAV_SAMPLE_BYTES;
}

// Finished when the whole data chunk has been accepted and played.
bool wav_stream_finished(struct WavStream* stream){
  return stream->data_remaining == 0 &&
    stream->pending_offset == stream->pending_end &&
    audio_buffered() <= 0;
}

// Release the device and the file.
void wav_stream_close(struct WavStream* stream){
  audio_close();
  if (stream->fd >= 0){
    close(stream->fd);
    stream->fd = -1;
  }
}
