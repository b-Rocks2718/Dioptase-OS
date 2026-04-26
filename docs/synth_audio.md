## Synth Audio Userland

This document describes the OS layer above the synth audio hardware registers
defined in `../../docs/mem_map.md`.

### MMIO Access

`get_synth_audio()` is trap `48`. It maps the physical synth audio page
`0x7FBC000..0x7FBCFFF` into the caller's user virtual address space with
read/write permission and returns an `unsigned*` pointer to the mapped page.

The kernel does not virtualize ownership of the synth device. If multiple user
processes write the synth page at the same time, the hardware sees those writes
in ordinary memory order and the audible result is process-order dependent.
Programs that need exclusive synth access should coordinate in user space.

The PCM WAV path remains separate: `play_audio_file()` still drives the PCM
ring buffer and the audio low-water interrupt. The synth device has no PCM
channel and no interrupt of its own.

### DSYN V1 File Format

DSYN is a compact command stream for the synth page. All integer fields are
little-endian 32-bit words.

Header, 32 bytes:

| Offset | Field | Meaning |
| --- | --- | --- |
| `0x00` | magic | ASCII `DSYN` |
| `0x04` | version | `1` |
| `0x08` | header_size | `32` |
| `0x0C` | sample_rate_hz | `25000` |
| `0x10` | event_count | Number of register-write events after the header |
| `0x14` | loop_start_event | `0xFFFFFFFF` means no loop; the current OS player rejects loops |
| `0x18` | flags | Must be `0` |
| `0x1C` | reserved | Must be `0` |

Each event is 12 bytes:

| Offset | Field | Meaning |
| --- | --- | --- |
| `+0x00` | delta_samples | Samples to wait after the previous event |
| `+0x04` | reg_offset | 32-bit aligned byte offset within the synth page |
| `+0x08` | value | Value written to that register |

`root/crt/synth_audio.c` validates the magic, version, header size, sample
rate, flags, loop marker, and event register alignment/range. It streams events
from the file descriptor and does not buffer the whole song. DSYN v1 register
offsets target ordinary synth control/channel registers; command-ring control
registers are transport registers used by the player and are rejected as DSYN event
targets.

### Timing

DSYN deltas are counted in 25 kHz synth samples so files match the hardware
audio rate. Synth device version 2 exposes `SYNTH_AUDIO_SAMPLE_COUNTER`, a
read-only 32-bit wrapping counter that advances on the same output-sample clock
that consumes synth audio. The DSYN player uses this counter only to keep its
command-ring enqueue window ahead of the renderer; event timing is carried by
the command records themselves.

Synth device version 4 adds a timestamped command ring at synth-page offset
`0x200`. DSYN playback requires this ring. The player writes 16-byte command
records into the MMIO ring, publishes batches by advancing
`SYNTH_AUDIO_CMD_WRITE_IDX`, and keeps a rolling 8192-sample command prebuffer
ahead of playback. The synth device consumes due records inside its sample
renderer before producing the target sample, so batched host rendering in
`EMU_AUDIO_FAST=yes` can still observe sample-accurate register changes within
each batch.

If the guest producer falls inside a 512-sample safety margin before publishing
an event, the player re-centers that event and following events 8192 samples
ahead of the current synth counter instead of queuing a command that the device
would have to report as late.

Older synth-device versions are not supported by the DSYN player. If the mapped
device reports a version below 4, playback fails with
`DSYN_ERR_SYNTH_RING_UNSUPPORTED` before queuing any commands.

### Programs

`/sbin/synth <file.dsyn>` validates and plays a DSYN file by mapping the synth
page with `get_synth_audio()`. It streams events into the hardware command
ring, waits for the ring to drain before returning, and reports ring
late/overflow/bad-offset/bad-flags status as DSYN playback errors. If parsing
or ring playback fails after playback starts, it resets the command ring and
disables the global synth enable bit so a malformed file does not leave queued
commands or a hanging note.

MIDI conversion is handled outside the guest by
`Dioptase-Emulators/Dioptase-Emulator-Full/scripts/midi_to_dsyn.py`.
