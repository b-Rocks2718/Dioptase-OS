#include "synth_audio.h"
#include "limits.h"
#include "stddef.h"
#include "sys.h"
#include "unistd.h"

#define DSYN_MAGIC_0 'D'
#define DSYN_MAGIC_1 'S'
#define DSYN_MAGIC_2 'Y'
#define DSYN_MAGIC_3 'N'
#define DSYN_READ_BUFFER_BYTES 1024u
/*
 * Refill when fewer than 256 unread bytes remain. DSYN events are 12 bytes, so
 * this leaves roughly twenty already-buffered events while a blocking disk read
 * is attempted only when the synth command ring is close to full.
 */
#define DSYN_READ_REFILL_THRESHOLD_BYTES 256u
/*
 * DSYN events store byte offsets from the MMIO contract. The playback code
 * forms byte addresses with unsigned arithmetic so register accesses do not
 * compile into division/multiplication helper calls.
 */

/*
 * User-mode assembly timing helper implemented in synth_audio_wait.s.
 * ABI contract from docs/abi.md:
 * - r1 carries the mapped synth MMIO page pointer.
 * - r2 carries the target 32-bit sample-counter value.
 * - the helper clobbers only caller-saved registers and performs no trap.
 */
void synth_audio_wait_until_sample_counter_asm(unsigned* regs,
    unsigned target_sample);

struct DsynReader {
  int fd;
  char buffer[DSYN_READ_BUFFER_BYTES];
  unsigned offset;
  unsigned available;
  int eof;
};

static unsigned synth_audio_read_reg(unsigned* regs, unsigned offset){
  return *((unsigned*)((unsigned)regs + offset));
}

static void synth_audio_write_reg(unsigned* regs, unsigned offset,
    unsigned value){
  *((unsigned*)((unsigned)regs + offset)) = value;
}

static unsigned read_u32_le(char* bytes){
  return (((unsigned)bytes[0]) & 0xFF) |
    ((((unsigned)bytes[1]) & 0xFF) << 8) |
    ((((unsigned)bytes[2]) & 0xFF) << 16) |
    ((((unsigned)bytes[3]) & 0xFF) << 24);
}

static int read_exact(int fd, char* buf, unsigned count){
  unsigned total = 0;

  while (total < count){
    int nread = read(fd, buf + total, count - total);
    if (nread < 0){
      return DSYN_ERR_READ;
    }
    if (nread == 0){
      return DSYN_ERR_TRUNCATED;
    }
    total += (unsigned)nread;
  }

  return DSYN_OK;
}

static void dsyn_reader_init(struct DsynReader* reader, int fd){
  reader->fd = fd;
  reader->offset = 0;
  reader->available = 0;
  reader->eof = 0;
}

static unsigned dsyn_reader_buffered_bytes(struct DsynReader* reader){
  return reader->available - reader->offset;
}

static int dsyn_reader_fill(struct DsynReader* reader){
  int nread;

  if (reader->eof){
    return DSYN_ERR_TRUNCATED;
  }
  nread = read(reader->fd, reader->buffer, DSYN_READ_BUFFER_BYTES);
  if (nread < 0){
    return DSYN_ERR_READ;
  }
  if (nread == 0){
    reader->eof = 1;
    return DSYN_ERR_TRUNCATED;
  }

  reader->offset = 0;
  reader->available = (unsigned)nread;
  return DSYN_OK;
}

static void dsyn_reader_compact(struct DsynReader* reader){
  unsigned remaining = dsyn_reader_buffered_bytes(reader);

  if (reader->offset == 0){
    return;
  }

  for (unsigned i = 0; i < remaining; ++i){
    reader->buffer[i] = reader->buffer[reader->offset + i];
  }
  reader->offset = 0;
  reader->available = remaining;
}

static int dsyn_reader_try_refill_tail(struct DsynReader* reader){
  int nread;

  if (reader->eof){
    return DSYN_OK;
  }
  if (reader->available == DSYN_READ_BUFFER_BYTES &&
      reader->offset == 0){
    return DSYN_OK;
  }

  dsyn_reader_compact(reader);
  nread = read(reader->fd, reader->buffer + reader->available,
    DSYN_READ_BUFFER_BYTES - reader->available);

  if (nread < 0){
    return DSYN_ERR_READ;
  }
  if (nread == 0){
    reader->eof = 1;
    return DSYN_OK;
  }

  reader->available += (unsigned)nread;
  return DSYN_OK;
}

static int dsyn_reader_read_exact(struct DsynReader* reader, char* buf,
    unsigned count){
  unsigned copied = 0;

  while (copied < count){
    if (reader->offset == reader->available){
      int rc = dsyn_reader_fill(reader);
      if (rc != DSYN_OK){
        return rc;
      }
    }

    unsigned chunk = reader->available - reader->offset;
    if (chunk > count - copied){
      chunk = count - copied;
    }

    for (unsigned i = 0; i < chunk; ++i){
      buf[copied + i] = reader->buffer[reader->offset + i];
    }
    reader->offset += chunk;
    copied += chunk;
  }

  return DSYN_OK;
}

static void copy_dsyn_header(struct DsynHeader* dest, struct DsynHeader* src){
  dest->version = src->version;
  dest->header_size = src->header_size;
  dest->sample_rate_hz = src->sample_rate_hz;
  dest->event_count = src->event_count;
  dest->loop_start_event = src->loop_start_event;
  dest->flags = src->flags;
  dest->reserved = src->reserved;
}

int synth_audio_read_dsyn_header_fd(int fd, struct DsynHeader* header){
  char bytes[DSYN_HEADER_BYTES];
  int rc;

  if (header == NULL){
    return DSYN_ERR_BAD_ARG;
  }

  rc = read_exact(fd, bytes, DSYN_HEADER_BYTES);
  if (rc != DSYN_OK){
    return rc;
  }

  if (bytes[0] != DSYN_MAGIC_0 || bytes[1] != DSYN_MAGIC_1 ||
      bytes[2] != DSYN_MAGIC_2 || bytes[3] != DSYN_MAGIC_3){
    return DSYN_ERR_BAD_MAGIC;
  }

  header->version = read_u32_le(bytes + 4);
  header->header_size = read_u32_le(bytes + 8);
  header->sample_rate_hz = read_u32_le(bytes + 12);
  header->event_count = read_u32_le(bytes + 16);
  header->loop_start_event = read_u32_le(bytes + 20);
  header->flags = read_u32_le(bytes + 24);
  header->reserved = read_u32_le(bytes + 28);

  return synth_audio_validate_dsyn_header(header);
}

int synth_audio_read_dsyn_event_fd(int fd, struct DsynEvent* event){
  char bytes[DSYN_EVENT_BYTES];
  int rc;

  if (event == NULL){
    return DSYN_ERR_BAD_ARG;
  }

  rc = read_exact(fd, bytes, DSYN_EVENT_BYTES);
  if (rc != DSYN_OK){
    return rc;
  }

  event->delta_samples = read_u32_le(bytes);
  event->reg_offset = read_u32_le(bytes + 4);
  event->value = read_u32_le(bytes + 8);

  return synth_audio_validate_dsyn_event(event);
}

static int synth_audio_read_dsyn_event_reader(struct DsynReader* reader,
    struct DsynEvent* event){
  char bytes[DSYN_EVENT_BYTES];
  int rc;

  rc = dsyn_reader_read_exact(reader, bytes, DSYN_EVENT_BYTES);
  if (rc != DSYN_OK){
    return rc;
  }

  event->delta_samples = read_u32_le(bytes);
  event->reg_offset = read_u32_le(bytes + 4);
  event->value = read_u32_le(bytes + 8);

  return synth_audio_validate_dsyn_event(event);
}

int synth_audio_validate_dsyn_header(struct DsynHeader* header){
  if (header == NULL){
    return DSYN_ERR_BAD_ARG;
  }

  if (header->version != DSYN_VERSION){
    return DSYN_ERR_BAD_VERSION;
  }
  if (header->header_size != DSYN_HEADER_BYTES){
    return DSYN_ERR_BAD_HEADER_SIZE;
  }
  if (header->sample_rate_hz != SYNTH_AUDIO_SAMPLE_RATE_HZ){
    return DSYN_ERR_BAD_SAMPLE_RATE;
  }
  if (header->loop_start_event != DSYN_NO_LOOP){
    return DSYN_ERR_UNSUPPORTED_LOOP;
  }
  if (header->flags != DSYN_FLAGS_NONE || header->reserved != 0){
    return DSYN_ERR_UNSUPPORTED_FLAGS;
  }

  return DSYN_OK;
}

int synth_audio_validate_dsyn_event(struct DsynEvent* event){
  if (event == NULL){
    return DSYN_ERR_BAD_ARG;
  }

  if ((event->reg_offset & 0x3) != 0 ||
      event->reg_offset >= SYNTH_AUDIO_CMD_STATUS_OFFSET){
    return DSYN_ERR_BAD_EVENT_OFFSET;
  }

  return DSYN_OK;
}

int synth_audio_scan_dsyn_fd(int fd, struct DsynStats* stats){
  struct DsynHeader header;
  struct DsynEvent event;
  unsigned total_samples = 0;
  unsigned max_delta_samples = 0;
  unsigned max_reg_offset = 0;
  int rc;

  if (stats == NULL){
    return DSYN_ERR_BAD_ARG;
  }

  rc = synth_audio_read_dsyn_header_fd(fd, &header);
  if (rc != DSYN_OK){
    return rc;
  }

  for (unsigned i = 0; i < header.event_count; ++i){
    rc = synth_audio_read_dsyn_event_fd(fd, &event);
    if (rc != DSYN_OK){
      return rc;
    }

    if (event.delta_samples > max_delta_samples){
      max_delta_samples = event.delta_samples;
    }
    if (event.reg_offset > max_reg_offset){
      max_reg_offset = event.reg_offset;
    }
    if (total_samples > UINT_MAX - event.delta_samples){
      total_samples = UINT_MAX;
    } else {
      total_samples += event.delta_samples;
    }
  }

  copy_dsyn_header(&stats->header, &header);
  stats->total_samples = total_samples;
  stats->max_delta_samples = max_delta_samples;
  stats->max_reg_offset = max_reg_offset;

  return DSYN_OK;
}

static unsigned synth_audio_ring_advance_idx(unsigned idx){
  idx += SYNTH_AUDIO_CMD_RECORD_BYTES;
  if (idx >= SYNTH_AUDIO_CMD_RING_BYTES){
    idx = 0;
  }
  return idx;
}

static unsigned synth_audio_ring_status_space(unsigned status){
  return status >> SYNTH_AUDIO_CMD_STATUS_SPACE_SHIFT;
}

static int synth_audio_ring_near_full(unsigned* regs){
  unsigned status = synth_audio_read_reg(regs, SYNTH_AUDIO_CMD_STATUS_OFFSET);

  /*
   * "Near full" means the device has at least all but one software publication
   * batch queued. That gives the player useful audio lead before taking a
   * potentially blocking disk read, without waiting for a completely full ring.
   */
  return synth_audio_ring_status_space(status) <=
    SYNTH_AUDIO_DSYN_RING_BATCH_COMMANDS;
}

static int maybe_refill_dsyn_reader(unsigned* regs,
    struct DsynReader* reader){
  if (dsyn_reader_buffered_bytes(reader) >=
      DSYN_READ_REFILL_THRESHOLD_BYTES){
    return DSYN_OK;
  }
  if (!synth_audio_ring_near_full(regs)){
    return DSYN_OK;
  }

  return dsyn_reader_try_refill_tail(reader);
}

static int synth_audio_ring_empty(unsigned* regs){
  return (synth_audio_read_reg(regs, SYNTH_AUDIO_CMD_STATUS_OFFSET) &
    SYNTH_AUDIO_CMD_STATUS_EMPTY) != 0;
}

static unsigned wait_until_synth_audio_ring_space(unsigned* regs){
  unsigned status;

  /*
   * Command-ring backpressure is not a scheduler sleep point. In normal
   * emulation, guest instructions advance device time; in EMU_AUDIO_FAST, the
   * host audio worker advances the ring independently. A syscall-heavy wait
   * here can make the writer lose its lead over the audio renderer.
   */
  for (;;){
    status = synth_audio_read_reg(regs, SYNTH_AUDIO_CMD_STATUS_OFFSET);
    if ((status & SYNTH_AUDIO_CMD_STATUS_FULL) == 0){
      return synth_audio_ring_status_space(status);
    }
  }
}

static void wait_until_synth_audio_ring_empty(unsigned* regs){
  while (!synth_audio_ring_empty(regs)){
  }
}

static int synth_audio_ring_status_error(unsigned status){
  if ((status & SYNTH_AUDIO_CMD_STATUS_BAD_FLAGS) != 0){
    return DSYN_ERR_SYNTH_RING_BAD_FLAGS;
  }
  if ((status & SYNTH_AUDIO_CMD_STATUS_BAD_OFFSET) != 0){
    return DSYN_ERR_SYNTH_RING_BAD_OFFSET;
  }
  if ((status & SYNTH_AUDIO_CMD_STATUS_OVERFLOW) != 0){
    return DSYN_ERR_SYNTH_RING_OVERFLOW;
  }
  if ((status & SYNTH_AUDIO_CMD_STATUS_LATE) != 0){
    return DSYN_ERR_SYNTH_RING_LATE;
  }

  return DSYN_OK;
}

static void wait_until_synth_audio_ring_enqueue_window(unsigned* regs,
    unsigned target_sample){
  /*
   * After the startup prefill, keep ring playback as a rolling prebuffer
   * instead of filling the whole ring as far into the song as possible.
   * Waiting until the event is within the configured lead keeps command
   * traffic distributed over playback while still giving the renderer enough
   * future work to apply events inside batched audio output.
   */
  unsigned enqueue_sample =
    target_sample - SYNTH_AUDIO_DSYN_RING_LEAD_SAMPLES;
  synth_audio_wait_until_sample_counter_asm(regs, enqueue_sample);
}

static int synth_audio_sample_reached(unsigned now, unsigned target){
  return (now - target) < 0x80000000u;
}

static unsigned synth_audio_keep_target_ahead(unsigned* regs,
    unsigned target_sample){
  unsigned now = synth_audio_read_reg(regs, SYNTH_AUDIO_SAMPLE_COUNTER_OFFSET);

  /*
   * If the producer fell behind, do not publish a command that the device will
   * necessarily report as late. Re-centering the target also moves following
   * events because the caller keeps adding DSYN deltas to the returned value.
   * This preserves ring-only playback while making audio-fast robust when the
   * host renderer advances wall-clock synth time faster than the guest can
   * parse a short command burst.
   */
  if (synth_audio_sample_reached(now, target_sample) ||
      target_sample - now < SYNTH_AUDIO_DSYN_RING_MIN_LEAD_SAMPLES){
    return now + SYNTH_AUDIO_DSYN_RING_LEAD_SAMPLES;
  }

  return target_sample;
}

static void synth_audio_reset_ring(unsigned* regs){
  synth_audio_write_reg(regs, SYNTH_AUDIO_CMD_CTRL_OFFSET,
    SYNTH_AUDIO_CMD_CTRL_RESET | SYNTH_AUDIO_CMD_CTRL_CLEAR_FLAGS);
}

static void synth_audio_publish_ring_write_idx(unsigned* regs,
    unsigned write_idx){
  synth_audio_write_reg(regs, SYNTH_AUDIO_CMD_WRITE_IDX_OFFSET, write_idx);
}

static void synth_audio_write_ring_command(unsigned* regs, unsigned write_idx,
    unsigned target_sample, unsigned reg_offset, unsigned value){
  synth_audio_write_reg(regs,
    SYNTH_AUDIO_CMD_RING_OFFSET + write_idx +
    SYNTH_AUDIO_CMD_TARGET_SAMPLE_OFFSET, target_sample);
  synth_audio_write_reg(regs,
    SYNTH_AUDIO_CMD_RING_OFFSET + write_idx + SYNTH_AUDIO_CMD_REG_OFFSET_OFFSET,
    reg_offset);
  synth_audio_write_reg(regs,
    SYNTH_AUDIO_CMD_RING_OFFSET + write_idx + SYNTH_AUDIO_CMD_VALUE_OFFSET,
    value);
  synth_audio_write_reg(regs,
    SYNTH_AUDIO_CMD_RING_OFFSET + write_idx + SYNTH_AUDIO_CMD_FLAGS_OFFSET, 0);
}

static void synth_audio_flush_ring_batch(unsigned* regs, unsigned write_idx,
    unsigned* staged_commands){
  if (*staged_commands == 0){
    return;
  }

  synth_audio_publish_ring_write_idx(regs, write_idx);
  *staged_commands = 0;
}

void synth_audio_stop(unsigned* regs){
  if (regs == NULL || regs == (unsigned*)-1){
    return;
  }

  synth_audio_reset_ring(regs);
  synth_audio_write_reg(regs, SYNTH_AUDIO_CTRL_OFFSET, 0);
}

/*
 * User-mode DSYN playback path.
 *
 * Preconditions:
 * - fd is positioned at the start of a DSYN v1 stream.
 * - get_synth_audio() maps a version-4-or-newer synth device page.
 *
 * Postconditions:
 * - On success, every DSYN event has been published through the command ring
 *   and the ring has drained.
 * - On parse or ring-status failure after the synth page is mapped, pending
 *   commands are cancelled and the global synth enable bit is cleared so a bad
 *   file cannot leave queued notes behind.
 */
int synth_audio_play_dsyn_fd(int fd){
  struct DsynHeader header;
  struct DsynEvent event;
  struct DsynReader reader;
  unsigned* regs;
  unsigned target_sample;
  unsigned synth_version;
  unsigned ring_write_idx = 0;
  unsigned ring_staged_commands = 0;
  unsigned ring_free_commands = 0;
  unsigned startup_prefill_remaining = 0;
  int rc;

  rc = synth_audio_read_dsyn_header_fd(fd, &header);
  if (rc != DSYN_OK){
    return rc;
  }

  regs = get_synth_audio();
  if (regs == NULL || regs == (unsigned*)-1){
    return DSYN_ERR_SYNTH_MAP;
  }

  dsyn_reader_init(&reader, fd);
  synth_version = synth_audio_read_reg(regs, SYNTH_AUDIO_VERSION_OFFSET);
  if (synth_version < SYNTH_AUDIO_COMMAND_RING_VERSION){
    return DSYN_ERR_SYNTH_RING_UNSUPPORTED;
  }
  target_sample = synth_audio_read_reg(regs, SYNTH_AUDIO_SAMPLE_COUNTER_OFFSET);
  synth_audio_reset_ring(regs);
  ring_write_idx =
    synth_audio_read_reg(regs, SYNTH_AUDIO_CMD_WRITE_IDX_OFFSET);
  ring_free_commands = synth_audio_ring_status_space(synth_audio_read_reg(
    regs, SYNTH_AUDIO_CMD_STATUS_OFFSET));
  startup_prefill_remaining = ring_free_commands;
  target_sample += SYNTH_AUDIO_DSYN_RING_LEAD_SAMPLES;

  for (unsigned i = 0; i < header.event_count; ++i){
    unsigned startup_prefill_event = startup_prefill_remaining != 0;

    rc = synth_audio_read_dsyn_event_reader(&reader, &event);
    if (rc != DSYN_OK){
      synth_audio_stop(regs);
      return rc;
    }

    target_sample += event.delta_samples;
    if (event.delta_samples != 0){
      synth_audio_flush_ring_batch(regs, ring_write_idx,
        &ring_staged_commands);
      rc = maybe_refill_dsyn_reader(regs, &reader);
      if (rc != DSYN_OK){
        synth_audio_stop(regs);
        return rc;
      }
    }
    if (!startup_prefill_event){
      wait_until_synth_audio_ring_enqueue_window(regs, target_sample);
    }
    target_sample = synth_audio_keep_target_ahead(regs, target_sample);
    if (ring_free_commands == 0){
      startup_prefill_remaining = 0;
      synth_audio_flush_ring_batch(regs, ring_write_idx,
        &ring_staged_commands);
      ring_free_commands = wait_until_synth_audio_ring_space(regs);
      target_sample = synth_audio_keep_target_ahead(regs, target_sample);
    }
    synth_audio_write_ring_command(regs, ring_write_idx, target_sample,
      event.reg_offset, event.value);
    ring_write_idx = synth_audio_ring_advance_idx(ring_write_idx);
    ring_staged_commands += 1;
    ring_free_commands -= 1;
    if (startup_prefill_remaining != 0){
      startup_prefill_remaining -= 1;
    }
    if (ring_staged_commands >= SYNTH_AUDIO_DSYN_RING_BATCH_COMMANDS){
      synth_audio_flush_ring_batch(regs, ring_write_idx,
        &ring_staged_commands);
      rc = maybe_refill_dsyn_reader(regs, &reader);
      if (rc != DSYN_OK){
        synth_audio_stop(regs);
        return rc;
      }
    }
  }

  synth_audio_flush_ring_batch(regs, ring_write_idx, &ring_staged_commands);
  wait_until_synth_audio_ring_empty(regs);
  rc = synth_audio_ring_status_error(synth_audio_read_reg(regs,
    SYNTH_AUDIO_CMD_STATUS_OFFSET));
  if (rc != DSYN_OK){
    synth_audio_stop(regs);
    return rc;
  }

  return DSYN_OK;
}

char* synth_audio_error_string(int err){
  if (err == DSYN_OK){
    return "ok";
  }
  if (err == DSYN_ERR_READ){
    return "read failed";
  }
  if (err == DSYN_ERR_TRUNCATED){
    return "file ended before the DSYN stream was complete";
  }
  if (err == DSYN_ERR_BAD_MAGIC){
    return "missing DSYN magic";
  }
  if (err == DSYN_ERR_BAD_VERSION){
    return "unsupported DSYN version";
  }
  if (err == DSYN_ERR_BAD_HEADER_SIZE){
    return "unsupported DSYN header size";
  }
  if (err == DSYN_ERR_BAD_SAMPLE_RATE){
    return "DSYN sample rate does not match the synth device";
  }
  if (err == DSYN_ERR_UNSUPPORTED_LOOP){
    return "DSYN looping is not supported by this player";
  }
  if (err == DSYN_ERR_UNSUPPORTED_FLAGS){
    return "DSYN flags or reserved header fields are unsupported";
  }
  if (err == DSYN_ERR_BAD_EVENT_OFFSET){
    return "DSYN event targets an invalid synth register offset";
  }
  if (err == DSYN_ERR_SYNTH_MAP){
    return "failed to map synth audio registers";
  }
  if (err == DSYN_ERR_BAD_ARG){
    return "invalid DSYN parser argument";
  }
  if (err == DSYN_ERR_SYNTH_RING_UNSUPPORTED){
    return "synth audio command-ring playback is not supported by this device";
  }
  if (err == DSYN_ERR_SYNTH_RING_LATE){
    return "synth audio command-ring command missed its target sample";
  }
  if (err == DSYN_ERR_SYNTH_RING_OVERFLOW){
    return "synth audio command ring overflowed before the player could enqueue";
  }
  if (err == DSYN_ERR_SYNTH_RING_BAD_OFFSET){
    return "synth audio command ring rejected an invalid target register offset";
  }
  if (err == DSYN_ERR_SYNTH_RING_BAD_FLAGS){
    return "synth audio command ring rejected unsupported command flags";
  }

  return "unknown DSYN error";
}
