"""SKYFORGE's 6 multi-minute 8-channel musical compositions.

Each piece is arranged across 5 to 7 distinct multi-bar sections (Intro, A, B,
A2 variation with counter-melody, Bridge/Breakdown, and Coda/Chorus) so every
song plays for 1.5 to 2.5 minutes before looping seamlessly.

Order of songs matches SONG_* in skyforge.h:
  1. title   — "Skyforge Overture" (Heroic D-minor / F-major symphonic synthwave)
  2. emerald — "Wings of Dioptase" (Upbeat A-Dorian canyon flight adventure)
  3. cyber   — "Neon Horizon" (High-speed E-minor Grand Prix & trench run)
  4. boss    — "Leviathan's Shadow" (Intense D-Phrygian dreadnought assault)
  5. forge   — "Crystalline Forge" (Lush G-major / E-minor world-sculptor ambient)
  6. victory — "Starlight Crown" (Triumphant D-major fanfare and celebration)
"""
from music import (Section, Song, CH_LEAD, CH_COUNTER, CH_PAD, CH_ARP, CH_BASS,
                   CH_LOW, CH_HIGH, CH_TEXTURE)

OFFBEATS = "..x...x...x...x."
EIGHTHS = "x.x.x.x.x.x.x.x."
BACKBEAT = "....x.......x..."


def title():
    prog_a = "Dm:4 Bb:4 F:4 C:4 Dm:4 Bb:4 Gm:4 A:4"
    mel_a = ("A4/4 D5/4 E5/4 F5/4 | G5/4. F5/8 D5/4 Bb4/4 | C5/4 F5/4 A5/2 | G5/2. E5/4 | "
             "A4/4 D5/4 E5/4 F5/4 | G5/4. A5/8 Bb5/4 A5/4 | G5/4 F5/4 E5/4 D5/4 | C#5/2 E5/2")

    intro = Section("intro", 4, "Dm:4 Bb:4 Gm:4 A:4")
    intro.pad(vel=26)
    intro.sustain(CH_TEXTURE, "choir", 22, "D4", degree=2)
    intro.arp("0 1 2 3 2 1 2 1", "harp", 28, low="D3")
    intro.note(CH_LOW, 0, 38, "timpani", 30).note(CH_LOW, 32, 43, "timpani", 26).note(CH_LOW, 48, 45, "timpani", 34)

    a = Section("A", 8, prog_a)
    a.melody(CH_LEAD, "horn", 46, mel_a)
    a.arp("0 1 2 3 2 1 2 1", "harp", 28, low="D3")
    a.pad(vel=24)
    a.bass("R...R...5...R...", vel=38, low="D2", inst="cello")
    a.drums(CH_LOW, "timpani", "x.......x.......", vel=26, note="D2")
    a.drums(CH_HIGH, "shaker", OFFBEATS, vel=18)

    b = Section("B", 8, "Bb:4 F:4 C:4 Dm:4 Bb:4 F:4 Gm:4 A:4")
    b.melody(CH_LEAD, "strings", 44,
             "F5/2 D5/2 | C5/2 A4/2 | G4/4 C5/4 E5/4 G5/4 | F5/2 D5/2 | "
             "F5/2 G5/4 F5/4 | F5/2 C5/2 | D5/4 E5/4 F5/4 G5/4 | A5/1")
    b.melody(CH_COUNTER, "celesta", 30,
             "r/1 | r/2 A5/4 C6/4 | r/1 | r/2 F5/4 A5/4 | "
             "r/1 | r/2 A5/4 C6/4 | r/1 | r/2 E5/4 C#6/4")
    b.pad(vel=24)
    b.sustain(CH_TEXTURE, "choir", 20, "F4", degree=1)
    b.arp("0 2 1 3 2 1 2 0", "harp", 24, low="D3")
    b.bass("R.R.5.R.R.R.O.5.", vel=40)
    b.drums(CH_LOW, "kick", "x.....x.x.....x.", vel=34)
    b.drums(CH_HIGH, "snare", BACKBEAT, vel=28)

    a2 = Section("A2", 8, prog_a)
    a2.melody(CH_LEAD, "flute", 46, mel_a, transpose=12)
    a2.melody(CH_COUNTER, "horn_soft", 32, mel_a)
    a2.arp("0 1 2 3 4 3 2 1", "celesta", 26, low="D4")
    a2.pad(vel=24)
    a2.bass("R.R.5.R.R.R.O.5.", vel=42)
    a2.drums(CH_LOW, "kick", "x...x...x...x...", vel=36)
    a2.drums(CH_HIGH, "snare", BACKBEAT, vel=30)

    c = Section("C", 8, "Gm:4 Dm:4 Bb:4 F:4 Gm:4 Dm:4 Edim:4 A:4")
    c.melody(CH_LEAD, "epiano", 42,
             "G4/4 Bb4/4 D5/2 | A4/4 D5/4 F5/2 | F4/4 Bb4/4 D5/4 F5/4 | C5/2 A4/2 | "
             "G4/4 Bb4/4 D5/2 | A4/4 D5/4 F5/2 | E5/4 D5/4 C#5/4 B4/4 | C#5/2 A4/2")
    c.sustain(CH_TEXTURE, "strings", 26, "D4")
    c.pad(vel=24)
    c.arp("0 1 2 1", "harp", 24, low="D3", rows_per_step=4)
    c.bass("R.......5.......", vel=36, low="D2", inst="cello")
    c.drums(CH_LOW, "timpani", "x.......x...x.x.", vel=28, note="A2")

    return Song("title", 96, [intro, a, b, a2, c],
                ["intro", "A", "B", "A2", "C", "B", "A2"], loop_to=1)


def emerald():
    prog_a = "Am:4 G:4 F:4 G:4 Am:4 G:4 F:4 E:4"
    mel_a = ("E5/4. D5/8 C5/4 A4/4 | B4/4. C5/8 D5/4 B4/4 | C5/4 A4/4 F5/4 E5/4 | D5/2 B4/2 | "
             "E5/4. D5/8 C5/4 E5/4 | G5/4. F5/8 E5/4 D5/4 | C5/4 D5/4 E5/4 F5/4 | E5/1")

    intro = Section("intro", 4, "Am:4 G:4 F:4 E:4")
    intro.arp("0 1 2 3 2 1 2 3", "epiano", 32, low="A3")
    intro.pad(vel=24)
    intro.bass("R.R.O.R.R.R.5.R.", vel=44)
    intro.drums(CH_LOW, "kick", "x...x...x...x...", vel=38)
    intro.drums(CH_HIGH, "snare", "....x.......x.xx", vel=32)

    a = Section("A", 8, prog_a)
    a.melody(CH_LEAD, "horn", 46, mel_a)
    a.arp("0 2 1 3 2 3 1 2", "harp", 28, low="A3")
    a.pad(vel=22)
    a.bass("R.R.O.R.R.R.5.R.", vel=44)
    a.drums(CH_LOW, "kick", "x.....x.x.....x.", vel=40)
    a.drums(CH_HIGH, "snare", BACKBEAT, vel=34)
    a.drums(CH_COUNTER, "hat", EIGHTHS, vel=18)

    b = Section("B", 8, "F:4 G:4 Em:4 Am:4 F:4 G:4 C:4 E:4")
    b.melody(CH_LEAD, "strings", 44,
             "A5/4. G5/8 F5/4 C5/4 | D5/4 G5/4 B5/2 | G5/4. F#5/8 E5/4 B4/4 | C5/4 E5/4 A5/2 | "
             "A5/4. G5/8 F5/4 A5/4 | B5/4. A5/8 G5/4 D5/4 | E5/4 G5/4 C6/4 B5/4 | G#5/1")
    b.melody(CH_COUNTER, "celesta", 30,
             "r/1 | r/2 D6/4 B5/4 | r/1 | r/2 E6/4 C6/4 | "
             "r/1 | r/2 G5/4 B5/4 | r/1 | r/2 B5/4 E6/4")
    b.arp("0 1 2 3 2 1 2 1", "epiano", 26, low="A3")
    b.pad(vel=24)
    b.bass("R.R.5.R.R.R.O.5.", vel=44)
    b.drums(CH_LOW, "kick", "x...x...x...x...", vel=40)
    b.drums(CH_HIGH, "snare", BACKBEAT, vel=34)
    b.sustain(CH_TEXTURE, "choir", 20, "E4", degree=1)

    a2 = Section("A2", 8, prog_a)
    a2.melody(CH_LEAD, "flute", 46, mel_a, transpose=12)
    a2.melody(CH_COUNTER, "horn_soft", 34, mel_a)
    a2.arp("0 1 2 3 4 3 2 1", "harp", 28, low="A3")
    a2.pad(vel=22)
    a2.bass("R.R.O.R.R.R.5.R.", vel=46)
    a2.drums(CH_LOW, "kick", "x.....x.x.....x.", vel=40)
    a2.drums(CH_HIGH, "snare", BACKBEAT, vel=36)
    a2.sustain(CH_TEXTURE, "strings", 22, "A4")

    c = Section("C", 8, "Dm:4 Am:4 F:4 G:4 Dm:4 Am:4 Bb:4 E:4")
    c.melody(CH_LEAD, "epiano", 42,
             "F5/4 D5/4 A4/2 | E5/4 C5/4 A4/2 | C5/4 F5/4 A5/4 G5/4 | B4/2 D5/2 | "
             "F5/4 D5/4 A4/2 | E5/4 A5/4 C6/2 | D6/4 C6/4 Bb5/4 A5/4 | G#5/2 E5/2")
    c.pad(vel=24)
    c.sustain(CH_TEXTURE, "choir", 24, "A4")
    c.arp("0 1 2 1", "harp", 26, low="A3", rows_per_step=4)
    c.bass("R.......5.......", vel=38, inst="cello", low="A1")
    c.drums(CH_HIGH, "shaker", EIGHTHS, vel=20)
    c.drums(CH_LOW, "timpani", "x.......x...x.x.", vel=30, note="A2")

    return Song("emerald", 124, [intro, a, b, a2, c],
                ["intro", "A", "B", "A2", "C", "B", "A2", "A"], loop_to=1)


def cyber():
    prog_a = "Em:4 C:4 D:4 Bm:4 Em:4 C:4 D:4 B:4"
    mel_a = ("B4/8 E5/8 G5/4 F#5/4 E5/4 | G5/4. E5/8 C5/2 | D5/8 F#5/8 A5/4 G5/4 F#5/4 | F#5/2 D5/2 | "
             "B4/8 E5/8 G5/4 B5/4 A5/4 | G5/4. E5/8 C5/4 E5/4 | F#5/4 G5/4 A5/4 G5/4 | F#5/1")

    intro = Section("intro", 4, "Em:4 C:4 D:4 B:4")
    intro.arp("0 1 2 3 0 1 2 3", "epiano", 34, low="E3", rows_per_step=1)
    intro.pad(vel=24, inst="pad_fifth")
    intro.bass("R.R.O.R.R.R.O.R.", vel=46)
    intro.drums(CH_LOW, "kick", "x...x...x...x...", vel=42)
    intro.drums(CH_HIGH, "hat", OFFBEATS, vel=22)

    a = Section("A", 8, prog_a)
    a.melody(CH_LEAD, "horn", 46, mel_a)
    a.arp("0 2 1 3 0 2 1 3", "epiano", 30, low="E3", rows_per_step=1)
    a.pad(vel=22)
    a.bass("R.O.R.O.R.O.5.O.", vel=46)
    a.drums(CH_LOW, "kick", "x...x...x...x...", vel=44)
    a.drums(CH_HIGH, "snare", BACKBEAT, vel=38)
    a.drums(CH_COUNTER, "hat", OFFBEATS, vel=20)

    b = Section("B", 8, "C:4 D:4 Em:4 Bm:4 C:4 D:4 G:4 B:4")
    b.melody(CH_LEAD, "strings", 46,
             "G5/2 E5/4 C5/4 | F#5/2 D5/4 A4/4 | B4/4 E5/4 G5/4 B5/4 | A5/2 F#5/2 | "
             "G5/2 E5/4 G5/4 | A5/2 F#5/4 A5/4 | B5/4 A5/4 G5/4 E5/4 | F#5/1")
    b.melody(CH_COUNTER, "musicbox", 32,
             "r/2 E6/4 G6/4 | r/2 D6/4 F#6/4 | r/2 B5/4 E6/4 | r/2 D6/4 A5/4 | "
             "r/2 E6/4 G6/4 | r/2 F#6/4 A6/4 | r/2 G6/4 B6/4 | r/2 F#6/4 D#6/4")
    b.arp("0 1 2 3 2 1 2 1", "harp", 28, low="E3")
    b.pad(vel=24)
    b.bass("R.R.O.R.R.R.5.R.", vel=46)
    b.drums(CH_LOW, "kick", "x...x...x...x...", vel=44)
    b.drums(CH_HIGH, "snare", BACKBEAT, vel=38)
    b.sustain(CH_TEXTURE, "choir", 22, "E4")

    a2 = Section("A2", 8, prog_a)
    a2.melody(CH_LEAD, "flute", 46, mel_a, transpose=12)
    a2.melody(CH_COUNTER, "horn", 34, mel_a)
    a2.arp("0 1 2 3 0 1 2 3", "celesta", 28, low="E4", rows_per_step=1)
    a2.pad(vel=22)
    a2.bass("R.O.R.O.R.O.5.O.", vel=46)
    a2.drums(CH_LOW, "kick", "x...x...x...x...", vel=44)
    a2.drums(CH_HIGH, "snare", BACKBEAT, vel=38)

    bridge = Section("bridge", 8, "Am:4 Em:4 C:4 D:4 Am:4 Em:4 F:4 B:4")
    bridge.melody(CH_LEAD, "celesta", 42,
                  "E5/2 C5/2 | B4/2 G4/2 |G4/4 C5/4 E5/4 G5/4 | F#5/2 D5/2 | "
                  "E5/2 A5/2 | G5/2 E5/2 | F5/4 E5/4 D5/4 C5/4 | B4/1")
    bridge.pad(vel=26)
    bridge.sustain(CH_TEXTURE, "strings", 24, "E4")
    bridge.arp("0 1 2 3 2 1 2 1", "harp", 26, low="E3")
    bridge.bass("R.......5.......", vel=38)
    bridge.drums(CH_HIGH, "shaker", EIGHTHS, vel=20)

    return Song("cyber", 134, [intro, a, b, a2, bridge],
                ["intro", "A", "B", "A2", "bridge", "B", "A2", "A"], loop_to=1)


def boss():
    prog_a = "Dm:4 Eb:4 Dm:4 Eb:4 Dm:4 C:4 Bb:4 A:4"
    mel_a = ("D4/4. Eb4/8 F4/4 D4/4 | Eb4/4 D4/4 C4/2 | D4/4. Eb4/8 F4/4 A4/4 | G4/4 F4/4 Eb4/2 | "
             "D4/8 F4/8 A4/8 D5/8 C5/4 A4/4 | Bb4/4 A4/4 G4/2 | F4/4 G4/4 A4/4 Bb4/4 | A4/1")

    intro = Section("intro", 4, "Dm:4 Eb:4 Dm:4 A:4")
    intro.pad(vel=28, inst="pad_fifth")
    intro.drums(CH_TEXTURE, "timpani", "X...x...x...x.x.", vel=38, note="D2")
    intro.note(CH_LOW, 0, 45, "gong", 36)
    intro.melody(CH_COUNTER, "choir_stab", 36,
                 "D4/4 r/4 D4/4 r/4 | Eb4/4 r/4 Eb4/4 r/4 | "
                 "D4/4 r/4 D4/4 r/4 | A4/4 r/4 A4/8 A4/8 Bb4/4")

    def groove(sec):
        sec.bass("R.R.R.R.R.R.O.R.", vel=48)
        sec.drums(CH_LOW, "kick", "x.....x.x.....x.", vel=46)
        sec.drums(CH_HIGH, "snare", "....X.......X..x", vel=40)
        sec.drums(CH_COUNTER, "hat", EIGHTHS, vel=18)
        sec.arp("0 2 1 2", "strings_fast", 28, low="D3")
        sec.pad(vel=22, inst="pad_fifth")

    a = Section("A", 8, prog_a)
    a.melody(CH_LEAD, "horn", 46, mel_a)
    groove(a)
    a.note(CH_TEXTURE, 0, 38, "timpani", 36).note(CH_TEXTURE, 64, 38, "timpani", 36)

    b = Section("B", 8, "Gm:4 Dm:4 Eb:4 Bb:4 Gm:4 Dm:4 Eb:4 A:4")
    b.melody(CH_LEAD, "choir", 44,
             "G4/2 Bb4/2 | A4/2 F4/2 | G4/2 Eb4/2 | F4/1 | "
             "G4/2 Bb4/2 | A4/2 D5/2 | Eb5/2 D5/4 C5/4 | C#5/1")
    groove(b)

    a2 = Section("A2", 8, prog_a)
    a2.melody(CH_LEAD, "horn", 48, mel_a, transpose=12)
    groove(a2)
    a2.sustain(CH_TEXTURE, "choir", 26, "A3")

    c = Section("C", 8, "Dm:4 Bb:4 Dm:4 Bb:4 Dm:4 C:4 Bb:4 A:4")
    c.melody(CH_LEAD, "strings", 44,
             "A4/1 | Bb4/1 | A4/2 F4/2 | D5/1 | "
             "A4/2 C5/2 | E5/2 C5/2 | D5/2 F5/2 | E5/1")
    c.pad(vel=26, inst="pad_fifth")
    c.drums(CH_TEXTURE, "timpani", "x...x...x.x.xxxx", vel=34, note="D2")
    c.bass("R.......R.......", vel=42)
    c.note(CH_COUNTER, 0, 45, "gong", 32)

    return Song("boss", 132, [intro, a, b, a2, c],
                ["intro", "A", "B", "A2", "C", "B", "A2"], loop_to=1)


def forge():
    prog = "G:4 D:4 Em:4 C:4 G:4 D:4 Am:4 D:4"
    mel = ("B4/4 D5/4 G5/2 | F#5/4 D5/4 A4/2 | G4/4 B4/4 E5/4 D5/4 | C5/2 E5/2 | "
           "B4/4 D5/4 G5/2 | A5/4 G5/4 F#5/4 D5/4 | E5/4 C5/4 A4/4 G4/4 | F#4/2 A4/2")

    intro = Section("intro", 4, "G:4 Em:4 C:4 D:4")
    intro.pad(vel=24)
    intro.arp("0 1 2 3 2 1 2 1", "harp", 28, low="G3")
    intro.sustain(CH_TEXTURE, "choir", 18, "G4", degree=1)

    a = Section("A", 8, prog)
    a.melody(CH_LEAD, "flute", 42, mel)
    a.arp("0 1 2 3 2 1 2 1", "harp", 26, low="G3")
    a.pad(vel=22)
    a.bass("R.......5.......", vel=32, low="G1", inst="cello")
    a.drums(CH_HIGH, "shaker", OFFBEATS, vel=14)

    b = Section("B", 8, "Em:4 Bm:4 C:4 G:4 Em:4 Bm:4 C:4 D:4")
    b.melody(CH_LEAD, "celesta", 42,
             "G5/2 E5/2 | F#5/2 D5/2 | E5/4 G5/4 C6/4 B5/4 | D5/1 | "
             "G5/2 B5/2 | F#5/2B5/2 | G5/4 E5/4 C5/4 E5/4 | F#5/1".replace("F#5/2B5/2", "F#5/2 B5/2"))
    b.sustain(CH_TEXTURE, "strings", 22, "G4")
    b.arp("0 2 1 3 2 1 2 0", "epiano", 24, low="G3")
    b.pad(vel=22)
    b.bass("R.......5.......", vel=32, low="G1", inst="cello")

    a2 = Section("A2", 8, prog)
    a2.melody(CH_LEAD, "strings", 40, mel)
    a2.melody(CH_COUNTER, "musicbox", 28, mel, transpose=12)
    a2.arp("0 1 2 3 2 1 2 1", "harp", 24, low="G3")
    a2.pad(vel=22)
    a2.bass("R.......5.......", vel=34, low="G1", inst="cello")

    return Song("forge", 88, [intro, a, b, a2],
                ["intro", "A", "B", "A2", "B", "A"], loop_to=1)


def victory():
    fanfare = Section("fanfare", 4, "D:4 G:4 A:4 D:4")
    fanfare.melody(CH_LEAD, "horn", 48,
                   "D4/8 F#4/8 A4/8 D5/8 A4/4 D5/4 | B4/4. G4/8 B4/4 D5/4 | "
                   "C#5/4 A4/4 E5/4 C#5/4 | D5/1")
    fanfare.sustain(CH_TEXTURE, "strings", 30, "F#4", degree=1)
    fanfare.pad(vel=28)
    fanfare.drums(CH_LOW, "timpani", "X...x...x...x...", vel=34, note="D2")
    fanfare.note(CH_COUNTER, 0, 45, "gong", 26)

    theme = "D:4 Bm:4 G:4 A:4 D:4 Bm:4 G:4 A:4"
    tune = ("F#5/4 A5/4 D6/2 | B5/4 A5/4 F#5/2 | G5/4 B5/4 D6/4 B5/4 | A5/1 | "
            "F#5/4 A5/4 D6/2 | B5/4 C#6/4 D6/4 B5/4 | G5/4 F#5/4 E5/4 G5/4 | A5/1")
    a = Section("theme", 8, theme)
    a.melody(CH_LEAD, "strings", 44, tune)
    a.arp("0 1 2 3 2 1 2 1", "harp", 28, low="D3")
    a.pad(vel=26)
    a.bass("R.......5.......", vel=40)
    a.drums(CH_LOW, "kick", "x.......x.......", vel=32)
    a.drums(CH_HIGH, "snare", BACKBEAT, vel=26)
    a.sustain(CH_TEXTURE, "choir", 22, "A4", degree=2)

    b = Section("calm", 8, theme)
    b.melody(CH_LEAD, "celesta", 42, tune)
    b.melody(CH_COUNTER, "flute", 34, tune)
    b.arp("0 1 2 3 2 1 2 1", "harp", 24, low="D3")
    b.pad(vel=24)
    b.bass("R.......5.......", vel=36)
    b.sustain(CH_TEXTURE, "choir", 20, "A4", degree=2)

    return Song("victory", 104, [fanfare, a, b],
                ["fanfare", "theme", "calm", "theme"], loop_to=1)


def build_songs():
    return [title(), emerald(), cyber(), boss(), forge(), victory()]
