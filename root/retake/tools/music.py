"""RETAKE's music compiler: waveforms, instruments and songs for the pak.

Synth model (audio.c): each music channel plays one instrument; a voice is a
phase accumulator stepping through a 256-entry waveform that audio.c scales
to 16 volume levels at startup, so mixing needs no multiplies. Envelopes run
at the 100 Hz control rate.

Song notation (songs.py): one string per channel, one token per row:
    C4 D#5 Bb3   start a note (sharps '#', flats 'b'; octave 4 holds middle C)
    -            hold the previous note
    .            release (note off)
    r            stay silent
Bars may be separated with '|' for readability (ignored).
"""
import math
import struct

SAMPLE_RATE = 25000
CONTROL_HZ = 100

NOTE_OFFSETS = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}

# Waveform ids (WAVE_* in audio.c)
WAVES = ["square", "pulse", "triangle", "saw", "piano", "organ", "sine", "reed"]


def wave_table(name):
    out = []
    for i in range(256):
        t = i / 256.0
        if name == "square":
            v = 1.0 if t < 0.5 else -1.0
        elif name == "pulse":
            v = 1.0 if t < 0.25 else -0.6
        elif name == "triangle":
            v = 4 * t - 1 if t < 0.5 else 3 - 4 * t
        elif name == "saw":
            v = 2 * t - 1
        elif name == "piano":
            v = (math.sin(2 * math.pi * t) + 0.55 * math.sin(4 * math.pi * t) +
                 0.32 * math.sin(6 * math.pi * t) + 0.18 * math.sin(8 * math.pi * t) +
                 0.08 * math.sin(10 * math.pi * t)) / 1.6
        elif name == "organ":
            v = (math.sin(2 * math.pi * t) + 0.6 * math.sin(4 * math.pi * t) +
                 0.4 * math.sin(8 * math.pi * t) + 0.2 * math.sin(16 * math.pi * t)) / 1.7
        elif name == "sine":
            v = math.sin(2 * math.pi * t)
        elif name == "reed":
            v = (math.sin(2 * math.pi * t) + 0.7 * math.sin(6 * math.pi * t) +
                 0.45 * math.sin(10 * math.pi * t) + 0.25 * math.sin(14 * math.pi * t)) / 1.8
        out.append(max(-1.0, min(1.0, v)))
    return [int(round(v * 1023)) for v in out]


def note_inc(midi):
    """32-bit phase increment per sample for a MIDI note."""
    freq = 440.0 * 2 ** ((midi - 69) / 12.0)
    return int(round(freq * (1 << 32) / SAMPLE_RATE)) & 0xFFFFFFFF


def parse_note(tok):
    letter = tok[0].upper()
    if letter not in NOTE_OFFSETS:
        raise ValueError(f"music: bad note {tok!r}")
    rest = tok[1:]
    acc = 0
    while rest and rest[0] in "#b":
        acc += 1 if rest[0] == "#" else -1
        rest = rest[1:]
    octave = int(rest)
    return 12 * (octave + 1) + NOTE_OFFSETS[letter] + acc


NOTE_HOLD = 0
NOTE_OFF = 1


def compile_channel(text):
    rows = []
    for tok in text.replace("|", " ").split():
        if tok == "-":
            rows.append(NOTE_HOLD)
        elif tok == ".":
            rows.append(NOTE_OFF)
        elif tok == "r":
            rows.append(NOTE_HOLD)
        else:
            rows.append(parse_note(tok))
    return rows


def compile_song(song, instruments):
    """song: dict(tempo=control ticks per row, channels=[(instrument name,
    text)]). Returns the packed song bytes: u16 rows, u16 channels,
    u16 ticks per row, u16 pad, channels x u8 instrument (padded to 4),
    then rows x channels note bytes."""
    chans = [compile_channel(t) for _, t in song["channels"]]
    n = max(len(c) for c in chans)
    for c in chans:
        while len(c) < n:
            c.append(NOTE_HOLD)
    inst_ids = [instruments.index(name) for name, _ in song["channels"]]
    head = struct.pack("<HHHH", n, len(chans), song["tempo"], 0)
    inst = bytes(inst_ids)
    while len(inst) % 4:
        inst += b"\0"
    body = bytearray()
    for r in range(n):
        for c in chans:
            body.append(c[r])
    while len(body) % 4:
        body.append(0)
    return head + inst + bytes(body)
