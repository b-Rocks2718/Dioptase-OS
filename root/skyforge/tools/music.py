"""SKYFORGE music engine and instrument synthesizer (build-time generator).

Synthesizes 21 rich 8-bit mu-law sampled instruments at 25 kHz (chord pads,
bowed strings, choir, flute, cello, brass horn, synth bass, Karplus-Strong
harp, FM celesta, electric piano, musicbox, and a full percussion kit) and
compiles multi-minute, 8-channel tracker arrangements into pattern streams
and `/skyforge/music.dat`.
"""
import numpy as np

RATE = 25000
CONTROL_HZ = 200
ENV_FULL = 16384
PITCH_CENTER = 64
MUSIC_CHANNELS = 8
RNG = np.random.default_rng(0x5F04E)


def midi_hz(note):
    return 440.0 * 2.0 ** ((note - 69) / 12.0)


NOTE_NAMES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def note_number(tok):
    semis = NOTE_NAMES[tok[0]]
    rest = tok[1:]
    while rest and rest[0] in "#b":
        semis += 1 if rest[0] == "#" else -1
        rest = rest[1:]
    return 12 * (int(rest) + 1) + semis


MULAW_BIAS = 0x84
MULAW_CLIP = 32635


def mulaw_encode(x):
    s = np.clip(np.round(np.asarray(x, dtype=np.float64) * 32767.0), -32768, 32767).astype(np.int64)
    sign = np.where(s < 0, 0x80, 0)
    mag = np.minimum(np.abs(s), MULAW_CLIP) + MULAW_BIAS
    exponent = np.clip(np.floor(np.log2(mag)).astype(np.int64) - 7, 0, 7)
    mantissa = (mag >> (exponent + 3)) & 0x0F
    return (~(sign | (exponent << 4) | mantissa)) & 0xFF


def mulaw_decode_table():
    table = []
    for code in range(256):
        u = ~code & 0xFF
        sign = u & 0x80
        exponent = (u >> 4) & 7
        mantissa = u & 0x0F
        s = (((mantissa << 3) + MULAW_BIAS) << exponent) - MULAW_BIAS
        table.append(-s if sign else s)
    return table


def times(seconds, rate):
    return np.arange(int(round(seconds * rate))) / rate


def harmonic_tone(f0, seconds, rate, amp_of_k, phase_seed=0, detune=0.0):
    t = times(seconds, rate)
    out = np.zeros_like(t)
    rng = np.random.default_rng(phase_seed)
    f = f0 * (1.0 + detune)
    k = 1
    while k * f < 0.45 * rate:
        a = amp_of_k(k)
        if a != 0.0:
            out += a * np.sin(2 * np.pi * k * f * t + rng.uniform(0, 2 * np.pi))
        k += 1
    return out


def saw_amps(rolloff_hz, f0):
    return lambda k: (1.0 / k) * np.exp(-k * f0 / rolloff_hz)


def one_pole_lowpass(x, cutoff_hz, rate):
    a = np.exp(-2 * np.pi * cutoff_hz / rate)
    y = np.zeros_like(x)
    acc = 0.0
    for i in range(len(x)):
        acc = (1 - a) * x[i] + a * acc
        y[i] = acc
    return y


def one_pole_highpass(x, cutoff_hz, rate):
    return x - one_pole_lowpass(x, cutoff_hz, rate)


def noise(seconds, rate, seed):
    return np.random.default_rng(seed).uniform(-1, 1, int(round(seconds * rate)))


def normalize(x, peak=0.97):
    m = np.max(np.abs(x))
    return x * (peak / m) if m > 0 else x


def crossfade_loop(x, loop_start, fade):
    x = x.copy()
    n = len(x)
    w = 0.5 - 0.5 * np.cos(np.linspace(0, np.pi, fade))
    x[n - fade:] = x[n - fade:] * (1 - w) + x[loop_start - fade:loop_start] * w
    return x


def ensemble(f0, seconds, rate, amp_of_k, voices=3, spread_cents=9.0, seed=1):
    out = 0
    for v in range(voices):
        cents = spread_cents * (v - (voices - 1) / 2) / max(1, (voices - 1) / 2)
        out = out + harmonic_tone(f0, seconds, rate, amp_of_k, seed + v, detune=2 ** (cents / 1200) - 1)
    return out / voices


def vibrato_tone(f0, seconds, rate, amp_of_k, vib_hz, vib_cents, seed):
    t = times(seconds, rate)
    dev = 2 ** (vib_cents * np.sin(2 * np.pi * vib_hz * t) / 1200)
    phase = 2 * np.pi * np.cumsum(f0 * dev) / rate
    out = np.zeros_like(t)
    rng = np.random.default_rng(seed)
    k = 1
    while k * f0 * 1.02 < 0.45 * rate:
        a = amp_of_k(k)
        if a != 0.0:
            out += a * np.sin(k * phase + rng.uniform(0, 2 * np.pi))
        k += 1
    return out


def formant_amps(f0, formants):
    def amp(k):
        f = k * f0
        a = 0.0
        for fc, bw, g in formants:
            a += g / (1.0 + ((f - fc) / bw) ** 2)
        return a * (1.0 / (1 + k * 0.02))
    return amp


def decay_env(seconds, rate, tau, attack=0.002):
    t = times(seconds, rate)
    env = np.exp(-t / tau)
    a = int(attack * rate)
    if a > 0:
        env[:a] *= np.linspace(0, 1, a)
    return env


class Instrument:
    def __init__(self, name, samples, rate, base_note, loop_start=None,
                 attack=0.005, decay=0.0, sustain=1.0, release=0.25, alias_of=None):
        self.name = name
        self.samples = samples
        self.rate = rate
        self.base_note = base_note
        self.loop_start = loop_start
        self.attack = attack
        self.decay = decay
        self.sustain = sustain
        self.release = release
        self.alias_of = alias_of


def alias(insts, name, of, **env):
    src = insts[of]
    insts[name] = Instrument(name, src.samples, src.rate, src.base_note, src.loop_start,
                             alias_of=of, **env)


def make_sustained(name, render, f0_note, rate, seconds, loop_from, fade, **env):
    x = normalize(render(midi_hz(f0_note), seconds, rate))
    ls = int(loop_from * rate)
    x = crossfade_loop(x, ls, int(fade * rate))
    base = f0_note + (12 if rate == RATE // 2 else 0)
    return Instrument(name, normalize(x), rate, base, loop_start=ls, **env)


def chord_pad(intervals, f_root, seconds, rate, seed):
    out = 0
    for i, iv in enumerate(intervals):
        f = f_root * 2 ** (iv / 12)
        out = out + ensemble(f, seconds, rate, saw_amps(1600 + 400 * i, f), voices=3,
                             spread_cents=11, seed=seed + 10 * i) * (1.0 / (1 + 0.15 * i))
    out = out + 0.35 * harmonic_tone(f_root / 2, seconds, rate, lambda k: 1.0 if k == 1 else 0.0, seed + 99)
    return one_pole_lowpass(out, 2600, rate)


def fade_tails(insts, seconds=0.012):
    for inst in insts.values():
        if inst.loop_start is None:
            n = max(2, int(seconds * inst.rate))
            inst.samples = inst.samples.copy()
            inst.samples[-n:] *= np.linspace(1, 0, n)


def build_instruments():
    insts = {}
    R = RATE

    def add(inst):
        insts[inst.name] = inst

    for name, ivs in (("pad_major", (0, 7, 12, 16, 19)), ("pad_minor", (0, 7, 12, 15, 19)),
                      ("pad_fifth", (0, 7, 12, 19, 24))):
        add(make_sustained(name, lambda f, s, r, ivs=ivs: chord_pad(ivs, f, s, r, 7 + len(name)),
                           note_number("C3"), R, 1.15, 0.28, 0.15,
                           attack=0.7, decay=0.0, sustain=1.0, release=1.2))
    add(make_sustained("strings", lambda f, s, r: one_pole_lowpass(
        sum(vibrato_tone(f * 2 ** (c / 1200), s, r, saw_amps(2400, f), 5.5, 9, 30 + i)
            for i, c in enumerate((-8, 0, 7))), 3400, r),
        note_number("G3"), R, 1.0, 0.25, 0.16, attack=0.25, sustain=0.9, decay=0.5, release=0.5))
    ah = ((780, 90, 1.0), (1150, 110, 0.55), (2600, 160, 0.18), (3300, 200, 0.08))
    add(make_sustained("choir", lambda f, s, r: sum(
        vibrato_tone(f * 2 ** (c / 1200), s, r, formant_amps(f, ah), 4.8, 14, 50 + i)
        for i, c in enumerate((-10, 0, 9))),
        note_number("G4"), R, 1.0, 0.25, 0.16, attack=0.35, sustain=1.0, release=0.7))

    def flute(f, s, r):
        tone = vibrato_tone(f, s, r, lambda k: {1: 1.0, 2: 0.24, 3: 0.09, 4: 0.04}.get(k, 0.0), 5.2, 11, 61)
        breath = one_pole_lowpass(one_pole_highpass(noise(s, r, 62), 600, r), 3000, r)
        return tone + 0.05 * breath
    add(make_sustained("flute", flute, note_number("A4"), R, 0.85, 0.28, 0.12,
                       attack=0.05, sustain=0.88, decay=0.2, release=0.18))
    add(make_sustained("cello", lambda f, s, r: one_pole_lowpass(
        vibrato_tone(f, s, r, saw_amps(950, f), 5.2, 10, 70) +
        0.6 * vibrato_tone(f * 1.003, s, r, saw_amps(950, f), 5.2, 10, 71), 1900, r),
        note_number("C3"), R, 0.85, 0.24, 0.13, attack=0.15, sustain=0.85, decay=0.4, release=0.4))
    add(make_sustained("horn", lambda f, s, r: one_pole_lowpass(
        ensemble(f, s, r, saw_amps(1500, f), voices=3, spread_cents=7, seed=80), 2300, r),
        note_number("F3"), R, 0.85, 0.28, 0.12, attack=0.05, sustain=0.85, decay=0.25, release=0.25))
    add(make_sustained("bass", lambda f, s, r: harmonic_tone(f, s, r, lambda k: 1.0 if k == 1 else 0.0, 90) +
                       0.55 * one_pole_lowpass(harmonic_tone(f, s, r, saw_amps(650, f), 91), 850, r),
                       note_number("C2"), R, 0.65, 0.2, 0.1, attack=0.005, sustain=0.78, decay=0.3, release=0.12))

    def pluck(f, s, r, seed, damping=0.996, bright=0.5):
        n = int(round(s * r))
        period = r / f
        d = int(np.floor(period))
        frac = period - d
        rng = np.random.default_rng(seed)
        line = one_pole_lowpass(rng.uniform(-1, 1, d + 2), f * (2 + 10 * bright), r)
        out = np.zeros(n)
        buf = list(line)
        for i in range(n):
            a = buf[-d]
            b = buf[-d - 1]
            v = (1 - frac) * a + frac * b
            nv = damping * 0.5 * (v + buf[-1])
            buf.append(nv)
            out[i] = v
            if len(buf) > 4 * d + 8:
                del buf[:len(buf) - 2 * d - 4]
        return out
    harp = normalize(pluck(midi_hz(note_number("C4")), 1.1, R, 100, damping=0.9975, bright=0.65) *
                     decay_env(1.1, R, 0.75))
    add(Instrument("harp", harp, R, note_number("C4"), attack=0.002, sustain=1.0, release=0.45))

    def fm_bell(f, s, r, ratio, index, tau, mod_tau):
        t = times(s, r)
        mod = index * np.exp(-t / mod_tau) * np.sin(2 * np.pi * f * ratio * t)
        return np.sin(2 * np.pi * f * t + mod) * decay_env(s, r, tau)
    celesta = fm_bell(midi_hz(note_number("C5")), 1.0, R, 3.5, 1.6, 0.45, 0.25) + \
        0.25 * np.sin(2 * np.pi * 2 * midi_hz(note_number("C5")) * times(1.0, R)) * decay_env(1.0, R, 0.15)
    add(Instrument("celesta", normalize(celesta), R, note_number("C5"), attack=0.001, release=0.5))
    epiano = fm_bell(midi_hz(note_number("C4")), 1.0, R, 1.0, 1.4, 0.55, 0.35) + \
        0.18 * fm_bell(midi_hz(note_number("C4")), 1.0, R, 14.0, 0.6, 0.05, 0.03)
    add(Instrument("epiano", normalize(epiano), R, note_number("C4"), attack=0.002, release=0.35))
    f_mb = midi_hz(note_number("C6"))
    t_mb = times(0.8, R)
    musicbox = sum(a * np.sin(2 * np.pi * f_mb * m * t_mb) * np.exp(-t_mb / tau)
                   for m, a, tau in ((1.0, 1.0, 0.45), (2.76, 0.35, 0.12), (5.4, 0.15, 0.05)))
    add(Instrument("musicbox", normalize(musicbox), R, note_number("C6"), attack=0.001, release=0.3))

    t = times(0.38, R)
    kick_f = 50 + 110 * np.exp(-t / 0.032)
    kick = np.sin(2 * np.pi * np.cumsum(kick_f) / R) * np.exp(-t / 0.12)
    click = one_pole_lowpass(noise(0.38, R, 110), 5000, R) * np.exp(-t / 0.004)
    kick += 0.5 * click + 0.25 * np.sin(2 * np.pi * 1600 * t) * np.exp(-t / 0.006)
    add(Instrument("kick", normalize(kick), R, note_number("C4"), attack=0.0, release=0.05))
    s = 0.30
    sn_noise = one_pole_highpass(one_pole_lowpass(noise(s, R, 120), 6500, R), 900, R) * decay_env(s, R, 0.09)
    sn_tone = np.sin(2 * np.pi * 190 * times(s, R)) * decay_env(s, R, 0.04)
    add(Instrument("snare", normalize(sn_noise + 0.6 * sn_tone), R, note_number("C4"), attack=0.0, release=0.05))
    hat = one_pole_highpass(noise(0.09, R, 130), 7000, R) * decay_env(0.09, R, 0.022)
    add(Instrument("hat", normalize(hat, 0.8), R, note_number("C4"), attack=0.0, release=0.02))
    sh = one_pole_highpass(one_pole_lowpass(noise(0.15, R, 140), 9000, R), 3500, R)
    sh *= np.concatenate([np.linspace(0, 1, int(0.03 * R)), np.exp(-times(0.12, R) / 0.035)])[:len(sh)]
    add(Instrument("shaker", normalize(sh, 0.7), R, note_number("C4"), attack=0.0, release=0.03))
    t = times(0.5, R)
    tom = np.sin(2 * np.pi * np.cumsum(95 + 60 * np.exp(-t / 0.06)) / R) * np.exp(-t / 0.20)
    add(Instrument("tom", normalize(tom), R, note_number("C4"), attack=0.0, release=0.1))
    t = times(1.1, R)
    f_t = midi_hz(note_number("G2"))
    timp = sum(a * np.sin(2 * np.pi * f_t * m * np.cumsum(1 + 0.03 * np.exp(-t / 0.05)) / R) * np.exp(-t / tau)
               for m, a, tau in ((1.0, 1.0, 0.85), (1.5, 0.45, 0.5), (1.98, 0.3, 0.35), (2.44, 0.15, 0.25)))
    timp[:240] += 0.25 * one_pole_lowpass(noise(1.1, R, 150), 2000, R)[:240] * np.linspace(1, 0, 240)
    add(Instrument("timpani", normalize(timp), R, note_number("G2"), attack=0.0, release=0.55))
    t = times(1.4, R)
    gong = sum(a * np.sin(2 * np.pi * 110 * m * t + 0.4 * np.sin(2 * np.pi * 0.7 * t * m)) * np.exp(-t / tau)
               for m, a, tau in ((1.0, 1.0, 1.2), (1.48, 0.7, 0.9), (2.13, 0.5, 0.7), (2.77, 0.45, 0.5)))
    gong[:200] += 0.3 * noise(1.4, R, 160)[:200] * np.linspace(1, 0, 200)
    add(Instrument("gong", normalize(gong), R, note_number("A2"), attack=0.0, release=0.9))
    click = one_pole_highpass(noise(0.05, R, 170), 2500, R) * decay_env(0.05, R, 0.008)
    add(Instrument("rim", normalize(click, 0.8), R, note_number("C4"), attack=0.0, release=0.02))
    fade_tails(insts)
    alias(insts, "strings_fast", "strings", attack=0.03, decay=0.2, sustain=0.55, release=0.15)
    alias(insts, "choir_stab", "choir", attack=0.04, decay=0.35, sustain=0.45, release=0.3)
    alias(insts, "horn_soft", "horn", attack=0.20, sustain=0.85, decay=0.4, release=0.45)
    return insts


ROWS_PER_BEAT = 4
CH_LEAD, CH_COUNTER, CH_PAD, CH_ARP, CH_BASS, CH_LOW, CH_HIGH, CH_TEXTURE = range(MUSIC_CHANNELS)

CHORD_SHAPES = {
    "": (0, 4, 7), "m": (0, 3, 7), "5": (0, 7, 12), "sus2": (0, 2, 7), "sus4": (0, 5, 7),
    "dim": (0, 3, 6), "aug": (0, 4, 8), "7": (0, 4, 7, 10), "m7": (0, 3, 7, 10),
    "maj7": (0, 4, 7, 11), "add9": (0, 4, 7, 14), "madd9": (0, 3, 7, 14), "m6": (0, 3, 7, 9),
}
PAD_FOR = {
    "": "pad_major", "7": "pad_major", "maj7": "pad_major", "add9": "pad_major", "aug": "pad_major",
    "m": "pad_minor", "m7": "pad_minor", "madd9": "pad_minor", "m6": "pad_minor", "dim": "pad_minor",
    "5": "pad_fifth", "sus2": "pad_fifth", "sus4": "pad_fifth",
}
DURATIONS = {"1": 16, "2": 8, "4": 4, "8": 2, "16": 1}
INSTRUMENT_GAIN = {"bass": 0.85, "kick": 1.25, "shaker": 1.35, "hat": 1.3, "rim": 1.3,
                   "harp": 1.1, "pad_major": 1.05, "pad_minor": 1.05, "pad_fifth": 1.05}


def parse_chord(sym):
    pc = NOTE_NAMES[sym[0]]
    i = 1
    while i < len(sym) and sym[i] in "#b":
        pc += 1 if sym[i] == "#" else -1
        i += 1
    quality = sym[i:]
    if quality not in CHORD_SHAPES:
        raise ValueError("unknown chord quality in %r" % sym)
    return pc % 12, quality


def parse_duration(text):
    total = 0
    for part in text.split("+"):
        dots = len(part) - len(part.rstrip("."))
        base = DURATIONS[part.rstrip(".")]
        total += base + (base // 2 if dots >= 1 else 0) + (base // 4 if dots >= 2 else 0)
    return total


def place(pc, low):
    return low + (pc - low) % 12


class Section:
    def __init__(self, name, bars, chords="", beats=4):
        self.name = name
        self.bars = bars
        self.beats = beats
        self.bar_rows = beats * ROWS_PER_BEAT
        self.rows = bars * self.bar_rows
        self.notes = []
        self.vols = []
        self.chords = []
        row = 0
        for tok in chords.split():
            sym, beats_text = tok.split(":")
            n = int(float(beats_text) * ROWS_PER_BEAT)
            pc, q = parse_chord(sym)
            self.chords.append((row, n, pc, q))
            row += n
        if self.chords and row != self.rows:
            raise ValueError("%s: chords cover %d rows, section has %d" % (name, row, self.rows))

    def chord_at(self, row):
        for start, n, pc, q in self.chords:
            if start <= row < start + n:
                return pc, q
        raise ValueError("%s: no chord at row %d" % (self.name, row))

    def note(self, ch, row, note, inst, vel, length=None):
        if not 0 <= row < self.rows:
            raise ValueError("%s: note at row %d outside the section" % (self.name, row))
        vel = min(63, int(round(vel * INSTRUMENT_GAIN.get(inst, 1.0))))
        self.notes.append((row, ch, note, inst, vel, length))
        return self

    def melody(self, ch, inst, vel, text, bar=0, transpose=0, legato=1.0):
        for b, bar_text in enumerate(text.split("|")):
            rows = sum(parse_duration(t.rstrip("!").split("/")[1]) for t in bar_text.split())
            if "|" in text and (rows == 0 or rows % self.bar_rows != 0):
                raise ValueError("%s: part %d of a melody lasts %d rows, not whole bars of %d: %r"
                                 % (self.name, b, rows, self.bar_rows, bar_text.strip()))
        row = bar * self.bar_rows
        for tok in text.replace("|", " ").split():
            accent = tok.endswith("!")
            tok = tok.rstrip("!")
            name, dur = tok.split("/")
            n = parse_duration(dur)
            if name != "r":
                length = max(1, int(round(n * legato)))
                self.note(ch, row, note_number(name) + transpose, inst, min(63, vel + (10 if accent else 0)), length)
            row += n
        return self

    def pad(self, vel=32, low="G2", ch=CH_PAD, inst=None):
        lo = note_number(low)
        for start, n, pc, q in self.chords:
            self.note(ch, start, place(pc, lo), inst or PAD_FOR[q], vel, n)
        return self

    def sustain(self, ch, inst, vel, low, degree=0):
        lo = note_number(low)
        for start, n, pc, q in self.chords:
            iv = CHORD_SHAPES[q][degree % len(CHORD_SHAPES[q])]
            self.note(ch, start, place((pc + iv) % 12, lo), inst, vel, n)
        return self

    def bass(self, pattern, vel=46, low="E1", inst="bass", ch=CH_BASS):
        lo = note_number(low)
        steps = pattern.replace(" ", "")
        for bar in range(self.bars):
            for i, c in enumerate(steps):
                row = bar * self.bar_rows + i * (self.bar_rows // len(steps))
                if c in ".":
                    continue
                if c == "-":
                    self.vols.append((row, ch, -1))
                    continue
                pc, q = self.chord_at(row)
                shape = CHORD_SHAPES[q]
                root = place(pc, lo)
                note = {"R": root, "5": root + 7, "O": root + 12,
                        "3": root + shape[1] if q not in ("5",) else root + 7,
                        "7": root + (shape[3] if len(shape) > 3 else 10)}[c]
                hold = 1
                while i + hold < len(steps) and steps[i + hold] == ".":
                    hold += 1
                self.note(ch, row, note, inst, vel, hold * (self.bar_rows // len(steps)))
        return self

    def arp(self, pattern, inst="harp", vel=30, low="C4", ch=CH_ARP, rows_per_step=2, legato=1.0):
        lo = note_number(low)
        toks = pattern.split()
        for start, n, pc, q in self.chords:
            shape = CHORD_SHAPES[q]
            tones = [place(pc, lo) + shape[i % len(shape)] + 12 * (i // len(shape)) for i in range(12)]
            step = 0
            for row in range(start, start + n, rows_per_step):
                t = toks[step % len(toks)]
                step += 1
                if t == "-":
                    continue
                self.note(ch, row, tones[int(t)], inst, vel, max(1, int(rows_per_step * legato)))
        return self

    def drums(self, ch, inst, pattern, vel=40, note="C4", bars=None):
        steps = pattern.replace(" ", "")
        per = self.bar_rows // len(steps)
        for bar in (range(self.bars) if bars is None else bars):
            for i, c in enumerate(steps):
                if c in "xXo":
                    v = vel + 10 if c == "X" else (vel // 2 if c == "o" else vel)
                    self.note(ch, bar * self.bar_rows + i * per, note_number(note), inst, min(63, v), None)
        return self


class Song:
    def __init__(self, name, bpm, sections, order, loop_to=0):
        self.name = name
        self.bpm = bpm
        self.sections = {s.name: s for s in sections}
        self.order = order
        self.loop_to = loop_to

    def row_samples(self):
        return int(round(RATE * 60.0 / self.bpm / ROWS_PER_BEAT))


EV_NOTE = 0x00
EV_OFF = 0x08
EV_VOL = 0x10
EV_WAIT = 0x80
EV_END = 0xFF


def compile_section(sec, inst_index):
    events = []
    by_ch = {}
    for row, ch, note, inst, vel, length in sec.notes:
        by_ch.setdefault(ch, []).append((row, note, inst, vel, length))
    for ch, notes in by_ch.items():
        notes.sort()
        for i, (row, note, inst, vel, length) in enumerate(notes):
            events.append((row, 1, bytes([EV_NOTE + ch, note, inst_index[inst], vel])))
            if length is None:
                continue
            end = min(row + length, sec.rows - 1)
            nxt = notes[i + 1][0] if i + 1 < len(notes) else None
            if nxt is not None and end >= nxt:
                continue
            if end <= row:
                continue
            events.append((end, 0, bytes([EV_OFF + ch])))
    for row, ch, vel in sec.vols:
        if vel < 0:
            events.append((row, 0, bytes([EV_OFF + ch])))
        else:
            events.append((row, 2, bytes([EV_VOL + ch, vel])))
    events.sort(key=lambda e: (e[0], e[1]))
    out = bytearray()
    row = 0
    for at, _, data in events:
        gap = at - row
        while gap > 0:
            n = min(gap, 128)
            out.append(EV_WAIT + n - 1)
            gap -= n
        row = at
        out += data
    gap = sec.rows - row
    while gap > 0:
        n = min(gap, 128)
        out.append(EV_WAIT + n - 1)
        gap -= n
    out.append(EV_END)
    return bytes(out)


class MusicData:
    def __init__(self, insts, songs):
        self.inst_names = list(insts)
        self.inst_index = {n: i for i, n in enumerate(self.inst_names)}
        self.samples = bytearray()
        self.inst_words = []
        offsets = {}
        for name in self.inst_names:
            inst = insts[name]
            if inst.alias_of is not None:
                src = insts[inst.alias_of]
                offset = offsets[inst.alias_of]
                length = len(src.samples)
                loop_start = src.loop_start if src.loop_start is not None else length
                self.inst_words += [offset, length, loop_start, src.base_note] + self.envelope_words(inst)
                continue
            codes = mulaw_encode(inst.samples)
            offset = len(self.samples)
            offsets[name] = offset
            self.samples += bytes(int(c) for c in codes)
            length = len(codes)
            loop_start = inst.loop_start if inst.loop_start is not None else length
            self.inst_words += [offset, length, loop_start, inst.base_note] + self.envelope_words(inst)
        self.patterns = []
        pattern_of = {}
        self.orders = bytearray()
        self.song_words = []
        for song in songs:
            first = len(self.orders)
            for sec_name in song.order:
                key = (song.name, sec_name)
                if key not in pattern_of:
                    pattern_of[key] = len(self.patterns)
                    self.patterns.append(compile_section(song.sections[sec_name], self.inst_index))
                self.orders.append(pattern_of[key])
            loop = -1 if song.loop_to is None else song.loop_to
            self.song_words += [song.row_samples(), first, len(song.order), loop]
        self.events = bytearray()
        self.pattern_offsets = []
        for p in self.patterns:
            self.pattern_offsets.append(len(self.events))
            self.events += p
        self.decode = mulaw_decode_table()
        self.pitch_steps = [int(round(2 ** ((i - PITCH_CENTER) / 12.0) * 65536)) for i in range(128)]

    @staticmethod
    def envelope_words(inst):
        attack = ENV_FULL if inst.attack <= 0 else max(1, int(ENV_FULL / (inst.attack * CONTROL_HZ)))
        decay = 0 if inst.decay <= 0 else max(1, int(ENV_FULL * (1 - inst.sustain) / (inst.decay * CONTROL_HZ)))
        release = ENV_FULL if inst.release <= 0 else max(1, int(ENV_FULL / (inst.release * CONTROL_HZ)))
        return [attack, decay, int(ENV_FULL * inst.sustain), release]


MUSIC_FILE = "/skyforge/music.dat"


def emit_music(out, data, emit_bytes, emit_words, emit_halfs):
    def words(label, values):
        out.append("  .global " + label)
        out.append(label + ":")
        emit_words(out, values)

    out.append("  .align 4")
    words("music_insts", data.inst_words)
    words("music_songs", data.song_words)
    words("music_pattern_offsets", data.pattern_offsets)
    words("pitch_steps", data.pitch_steps)
    emit_halfs(out, "mulaw_table", [v & 0xFFFF for v in data.decode])
    out.append("  .align 4")
    emit_bytes(out, "music_orders", data.orders)
    out.append("  .align 4")
    emit_bytes(out, "music_events", data.events)
    out.append("  .align 4")


def write_samples(path, data):
    with open(path, "wb") as f:
        f.write(bytes(data.samples))


def header_text(data):
    return "\n".join([
        "/* Generated by tools/gen_assets.py -- do not edit by hand. */",
        "#ifndef MUSIC_DATA_H",
        "#define MUSIC_DATA_H",
        "",
        "#define MUSIC_CHANNELS %d" % MUSIC_CHANNELS,
        "#define MUSIC_INSTRUMENTS %d" % len(data.inst_names),
        "#define MUSIC_SONGS %d" % (len(data.song_words) // 4),
        "",
        "extern int music_insts[%d];" % len(data.inst_words),
        "extern int music_songs[%d];" % len(data.song_words),
        "extern int music_pattern_offsets[%d];" % len(data.pattern_offsets),
        "extern unsigned pitch_steps[128];",
        "extern short mulaw_table[256];",
        "extern unsigned char music_orders[%d];" % len(data.orders),
        "extern unsigned char music_events[%d];" % len(data.events),
        "",
        "#define MUSIC_FILE \"%s\"" % MUSIC_FILE,
        "#define MUSIC_SAMPLE_BYTES %d" % len(data.samples),
        "",
        "#endif /* MUSIC_DATA_H */",
        "",
    ])
