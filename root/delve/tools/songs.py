"""DELVE's songs, arranged for the sample tracker in music.py.

build_songs() returns them in the order of the MUSIC_* ids in delve.h:
title, victory, death, the Warden, then one theme per depth 1..7. Each depth
theme is built from an intro and several sixteen- or eight-bar sections with
variations (a new lead instrument, a counter-melody, a breakdown), so it runs
one and a half to two and a half minutes before looping back past its intro.

Channels (music.py): CH_LEAD melody, CH_COUNTER second voice or extra
percussion, CH_PAD chord pads, CH_ARP arpeggios and ostinatos, CH_BASS,
CH_LOW kick/timpani/gong, CH_HIGH snare/hats/shaker/rim, CH_TEXTURE
sustained choir or strings. Velocities run 0..63.
"""
from music import (Section, Song, CH_LEAD, CH_COUNTER, CH_PAD, CH_ARP, CH_BASS,
                   CH_LOW, CH_HIGH, CH_TEXTURE)

OFFBEATS = "..x...x...x...x."
EIGHTHS = "x.x.x.x.x.x.x.x."
BACKBEAT = "....x.......x..."


# ---------------------------------------------------------------------------
# Title: "Into the Dioptase Deep" -- D minor, slow and wide.
# ---------------------------------------------------------------------------
def title():
    main = "Dm:4 Bb:4 F:4 C:4 Dm:4 Bb:4 Gm:4 A:4"
    theme = ("A4/4 D5/4 E5/4 F5/4 | G5/4. F5/8 D5/4 Bb4/4 | C5/4 F5/4 A5/2 | G5/2. r/4 | "
             "A4/4 D5/4 E5/4 F5/4 | G5/4. A5/8 Bb5/4 A5/4 | G5/4 F5/4 E5/4 D5/4 | C#5/2 E5/2")

    intro = Section("intro", 4, "Dm:4 Bb:4 Gm:4 A:4")
    intro.pad(vel=24)
    intro.sustain(CH_TEXTURE, "choir", 20, "D4", degree=2)
    intro.note(CH_LOW, 0, 38, "timpani", 28).note(CH_LOW, 32, 43, "timpani", 24).note(CH_LOW, 56, 45, "timpani", 32)
    intro.melody(CH_ARP, "harp", 30, "A3/8 C#4/8 E4/8 A4/8 C#5/8 E5/8 A5/4", bar=3)

    a = Section("A", 8, main)
    a.melody(CH_LEAD, "celesta", 46, theme)
    a.arp("0 1 2 3 2 1 2 1", "harp", 26, low="D3")
    a.pad(vel=24)
    a.bass("R" + "." * 15, vel=34, low="D2", inst="cello")
    a.note(CH_LOW, 0, 38, "timpani", 24).note(CH_LOW, 64, 38, "timpani", 24)

    b = Section("B", 8, "Bb:4 F:4 Gm:4 Dm:4 Bb:4 F:4 Gm:4 A:4")
    b.melody(CH_LEAD, "strings", 40, "F5/2 D5/2 | C5/2 A4/2 | Bb4/4 C5/4 D5/4 G5/4 | F5/1 | "
                                     "F5/2 G5/4 F5/4 | F5/2 C5/2 | D5/4 E5/4 F5/4 G5/4 | A5/1")
    b.melody(CH_COUNTER, "celesta", 28, "r/1 | r/2 A5/4 C6/4 | r/1 | r/2 F5/4 A5/4 | "
                                        "r/1 | r/2 A5/4 C6/4 | r/1 | r/2 E5/4 C#6/4")
    b.pad(vel=24)
    b.sustain(CH_TEXTURE, "choir", 18, "F4", degree=1)
    b.arp("0 1 2 1", "harp", 22, low="D3", rows_per_step=4)
    b.bass("R" + "." * 15, vel=30, low="D2", inst="cello")
    b.note(CH_LOW, 0, 46, "timpani", 24).note(CH_LOW, 64, 46, "timpani", 24).note(CH_LOW, 120, 45, "timpani", 32)

    a2 = Section("A2", 8, main)
    a2.melody(CH_LEAD, "flute", 44, theme)
    a2.melody(CH_COUNTER, "celesta", 26, "r/1 | r/2 D6/8 Bb5/8 G5/4 | r/1 | r/2 E6/8 C6/8 G5/4 | "
                                         "r/1 | r/2 D6/8 Bb5/8 F5/4 | r/1 | r/2 A5/8 C#6/8 E6/4")
    a2.arp("0 1 2 3 2 1 2 1", "harp", 24, low="D3")
    a2.pad(vel=24)
    a2.bass("R" + "." * 15, vel=32, low="D2", inst="cello")
    a2.drums(CH_HIGH, "shaker", OFFBEATS, vel=16)
    a2.note(CH_LOW, 0, 38, "timpani", 24).note(CH_LOW, 64, 38, "timpani", 24)

    bridge = Section("bridge", 4, "Dm:4 Bb:4 C:4 A:4")
    bridge.melody(CH_LEAD, "horn_soft", 38, "D4/2 A4/2 | Bb4/2 F4/2 | C5/2 G4/4 E4/4 | A4/1")
    bridge.sustain(CH_TEXTURE, "strings", 22, "D4")
    bridge.pad(vel=26)
    bridge.bass("R" + "." * 15, vel=32, low="D2", inst="cello")
    bridge.drums(CH_LOW, "timpani", "x.x.x.x.xxxxXXXX", vel=20, note="A2", bars=[3])
    return Song("title", 72, [intro, a, b, a2, bridge], ["intro", "A", "B", "A2", "bridge"], loop_to=1)


# ---------------------------------------------------------------------------
# Victory: "The Heart of Dioptase" -- D major fanfare, then a gentle loop.
# ---------------------------------------------------------------------------
def victory():
    fanfare = Section("fanfare", 4, "D:4 G:4 A:4 D:4")
    fanfare.melody(CH_LEAD, "horn", 48, "D4/8 F#4/8 A4/8 D5/8 A4/4 D5/4 | B4/4. G4/8 B4/4 D5/4 | "
                                        "C#5/4 A4/4 E5/4 C#5/4 | D5/1")
    fanfare.sustain(CH_TEXTURE, "strings", 30, "F#4", degree=1)
    fanfare.pad(vel=28)
    fanfare.drums(CH_LOW, "timpani", "X...x...x...x...", vel=34, note="D2", bars=[0, 1, 2])
    fanfare.drums(CH_LOW, "timpani", "xxxxxxxxXXXXX...", vel=26, note="A2", bars=[3])
    fanfare.note(CH_COUNTER, 0, 45, "gong", 24)

    theme = "D:4 Bm:4 G:4 A:4 D:4 Bm:4 G:4 A:4"
    tune = ("F#5/4 A5/4 D6/2 | B5/4 A5/4 F#5/2 | G5/4 B5/4 D6/4 B5/4 | A5/1 | "
            "F#5/4 A5/4 D6/2 | B5/4 C#6/4 D6/4 B5/4 | G5/4 F#5/4 E5/4 G5/4 | A5/1")
    a = Section("theme", 8, theme)
    a.melody(CH_LEAD, "strings", 42, tune)
    a.arp("0 1 2 3 2 1 2 1", "harp", 28, low="D3")
    a.pad(vel=26)
    a.bass("R.......5.......", vel=40)
    a.drums(CH_LOW, "kick", "x.......x.......", vel=30)
    a.drums(CH_HIGH, "snare", BACKBEAT, vel=24)
    a.sustain(CH_TEXTURE, "choir", 20, "A4", degree=2)

    coda = Section("coda", 4, "G:4 A:4 D:8")
    coda.melody(CH_LEAD, "horn", 46, "G4/4 B4/4 D5/2 | A4/4 C#5/4 E5/2 | F#5/1+1")
    coda.sustain(CH_TEXTURE, "strings", 30, "B4", degree=1)
    coda.pad(vel=28)
    coda.bass("R" + "." * 15, vel=40)
    coda.note(CH_LOW, 32, 38, "timpani", 40).note(CH_COUNTER, 32, 45, "gong", 30)

    calm = Section("calm", 8, theme)
    calm.melody(CH_LEAD, "celesta", 40, tune)
    calm.arp("0 1 2 3 2 1 2 1", "harp", 24, low="D3")
    calm.pad(vel=24)
    calm.bass("R.......5.......", vel=34)
    calm.sustain(CH_TEXTURE, "choir", 18, "A4", degree=2)
    return Song("victory", 100, [fanfare, a, coda, calm], ["fanfare", "theme", "coda", "calm"], loop_to=3)


# ---------------------------------------------------------------------------
# Death: "Fallen" -- a short lament, played once.
# ---------------------------------------------------------------------------
def death():
    lament = Section("lament", 4, "Dm:4 Gm:4 A:4 Dm:4")
    lament.melody(CH_LEAD, "cello", 42, "D4/4. C4/8 Bb3/4 A3/4 | G3/2 Bb3/2 | A3/2. C#4/4 | D4/1")
    lament.sustain(CH_TEXTURE, "choir", 24, "D4")
    lament.pad(vel=22)
    lament.note(CH_LOW, 0, 45, "gong", 26)
    return Song("death", 56, [lament], ["lament"], loop_to=None)


# ---------------------------------------------------------------------------
# The Warden: E Phrygian, fast and heavy.
# ---------------------------------------------------------------------------
def boss():
    hats = "x.x.x.x.x.x.x.x."
    intro = Section("intro", 4, "Em:4 F:4 Em:4 F:4")
    intro.pad(vel=26, inst="pad_fifth")
    intro.drums(CH_TEXTURE, "timpani", "X...x...x...x.x.", vel=36, note="E2")
    intro.note(CH_LOW, 0, 45, "gong", 34)
    intro.melody(CH_COUNTER, "choir_stab", 34, "E4/4 r/4 E4/4 r/4 | F4/4 r/4 F4/4 r/4 | "
                                               "E4/4 r/4 E4/4 r/4 | F4/4 r/4 F4/8 F4/8 G4/4")

    prog_a = "Em:4 F:4 Em:4 F:4 Em:4 D:4 C:4 B:4"
    lead_a = ("E4/4. F4/8 G4/4 E4/4 | F4/4 E4/4 D4/2 | E4/4. F4/8 G4/4 B4/4 | A4/4 G4/4 F4/2 | "
              "E4/8 G4/8 B4/8 E5/8 D5/4 B4/4 | C5/4 B4/4 A4/2 | G4/4 A4/4 B4/4 C5/4 | B4/1")

    def groove(sec):
        sec.bass("R.R.R.R.R.R.O.R.", vel=46)
        sec.drums(CH_LOW, "kick", "x.....x.x.....x.", vel=44)
        sec.drums(CH_HIGH, "snare", "....X.......X..x", vel=38)
        sec.drums(CH_COUNTER, "hat", hats, vel=16)
        sec.arp("0 2 1 2", "strings_fast", 26, low="E3")
        sec.pad(vel=20, inst="pad_fifth")

    a = Section("A", 8, prog_a)
    a.melody(CH_LEAD, "horn", 44, lead_a)
    groove(a)
    a.note(CH_TEXTURE, 0, 40, "timpani", 34).note(CH_TEXTURE, 64, 40, "timpani", 34)

    b = Section("B", 8, "Am:4 Em:4 F:4 C:4 Am:4 Em:4 F:4 B:4")
    b.melody(CH_LEAD, "choir", 40, "A4/2 C5/2 | B4/2 G4/2 | A4/2 F4/2 | G4/1 | A4/2 C5/2 | B4/2 E5/2 | "
                                   "F5/2 E5/4 D5/4 | D#5/1")
    groove(b)
    b.drums(CH_TEXTURE, "timpani", "x...............", vel=30, note="E2")

    a2 = Section("A2", 8, prog_a)
    a2.melody(CH_LEAD, "horn", 46, lead_a, transpose=12)
    groove(a2)
    a2.sustain(CH_TEXTURE, "choir", 24, "B3")

    c = Section("C", 8, "Em:4 C:4 Em:4 C:4 Em:4 D:4 C:4 B:4")
    c.melody(CH_LEAD, "strings", 40, "B4/1 | C5/1 | B4/2 G4/2 | E5/1 | B4/2 D5/2 | F#5/2 D5/2 | "
                                     "E5/2 G5/2 | F#5/1")
    c.pad(vel=24, inst="pad_fifth")
    c.drums(CH_TEXTURE, "timpani", "x.......x.......", vel=30, note="E2", bars=range(4))
    c.drums(CH_TEXTURE, "timpani", "x...x...x.x.xxxx", vel=32, note="E2", bars=range(4, 8))
    c.drums(CH_LOW, "tom", "........x.x.x.xx", vel=34, bars=[6, 7])
    c.bass("R.......R.......", vel=40)
    c.note(CH_COUNTER, 0, 45, "gong", 30)
    return Song("boss", 132, [intro, a, b, a2, c], ["intro", "A", "B", "A2", "C", "B"], loop_to=1)


# ---------------------------------------------------------------------------
# Depth 1: "The Upper Galleries" -- A Dorian, light and adventurous.
# ---------------------------------------------------------------------------
def depth1():
    prog_a = "Am:4 D:4 Am:4 D:4 F:4 G:4 Am:4 E:4"
    tune_a = ("E5/8 A5/8 B5/8 C6/8 B5/4 A5/4 | F#5/4. E5/8 D5/4 E5/4 | "
              "C5/8 E5/8 A5/8 B5/8 C6/4 B5/8 A5/8 | F#5/2 r/4 A4/4 | "
              "C5/4 F5/4 A5/4 G5/8 F5/8 | G5/4. F5/8 E5/4 D5/4 | E5/4 C5/4 A4/4 C5/4 | B4/2 G#4/4 E4/4")
    prog_b = "F:4 G:4 Em:4 Am:4 Dm:4 G:4 C:4 E:4"
    tune_b = "A4/2 C5/2 | B4/2 D5/2 | E5/2 G5/4 E5/4 | A5/1 | F5/2 A5/2 | G5/2 D5/2 | E5/2 G5/2 | G#5/1"

    def bed(sec, ostinato="0 1 2 1 3 1 2 1"):
        sec.arp(ostinato, "harp", 24, low="A3")
        sec.pad(vel=20)

    intro = Section("intro", 4, "Am:4 D:4 Am:4 D:4")
    bed(intro)
    intro.drums(CH_HIGH, "shaker", OFFBEATS, vel=14, bars=[2, 3])

    a = Section("A", 8, prog_a)
    a.melody(CH_LEAD, "flute", 42, tune_a)
    bed(a)
    a.bass("R...R.5.R...O.5.", vel=40)
    a.drums(CH_LOW, "kick", "x.......x.......", vel=30)
    a.drums(CH_HIGH, "shaker", OFFBEATS, vel=14)

    b = Section("B", 8, prog_b)
    b.melody(CH_LEAD, "strings", 38, tune_b)
    b.melody(CH_COUNTER, "celesta", 24, "r/2 C6/4 A5/4 | r/2 D6/4 B5/4 | r/2 B5/4 G5/4 | r/2 E6/4 C6/4 | "
                                        "r/2 D6/4 A5/4 | r/2 B5/4 G5/4 | r/2 C6/4 G5/4 | r/2 E6/4 B5/4")
    bed(b, "0 2 1 3 2 4 3 1")
    b.bass("R.......5.......", vel=38)
    b.drums(CH_LOW, "kick", "x.......x.......", vel=28)
    b.drums(CH_HIGH, "rim", BACKBEAT, vel=18)

    a2 = Section("A2", 8, prog_a)
    a2.melody(CH_LEAD, "flute", 42, tune_a)
    a2.melody(CH_COUNTER, "celesta", 26, "r/1 | r/2 A5/8 F#5/8 D5/4 | r/1 | r/2 E5/8 F#5/8 A5/4 | "
                                         "r/1 | r/2 D6/8 B5/8 G5/4 | r/1 | r/2 E5/8 G#5/8 B5/4")
    bed(a2)
    a2.bass("R...R.5.R...O.5.", vel=40)
    a2.drums(CH_LOW, "kick", "x.......x.x.....", vel=30)
    a2.drums(CH_HIGH, "shaker", EIGHTHS, vel=14)

    c = Section("C", 8, "Dm:4 Am:4 Dm:4 E:4 F:4 C:4 Dm:4 E:4")
    c.melody(CH_LEAD, "epiano", 40, "D5/4 F5/4 A5/4 F5/4 | E5/2 C5/2 | D5/4 F5/4 A5/4 D6/4 | B5/2 G#5/2 | "
                                    "A5/4 G5/4 F5/4 A5/4 | G5/4 E5/4 C5/2 | D5/4 E5/4 F5/4 D5/4 | E5/1")
    c.arp("0 1 2 1", "harp", 22, low="A3", rows_per_step=4)
    c.pad(vel=22)
    c.sustain(CH_TEXTURE, "choir", 18, "C5", degree=1)
    c.bass("R" + "." * 15, vel=38)

    b2 = Section("B2", 8, prog_b)
    b2.melody(CH_LEAD, "flute", 40, tune_b, transpose=12)
    b2.sustain(CH_TEXTURE, "strings", 22, "E4", degree=2)
    bed(b2, "0 2 1 3 2 4 3 1")
    b2.bass("R.......5.......", vel=38)
    b2.drums(CH_LOW, "kick", "x.......x.......", vel=28)
    b2.drums(CH_HIGH, "rim", BACKBEAT, vel=18)

    outro = Section("outro", 4, "Am:4 D:4 F:4 E:4")
    outro.melody(CH_LEAD, "flute", 38, "E5/2 A5/2 | F#5/1 | A5/2 C6/2 | B5/2 G#5/2")
    bed(outro)
    outro.bass("R" + "." * 15, vel=34)
    return Song("depth1", 100, [intro, a, b, a2, c, b2, outro],
                ["intro", "A", "B", "A2", "C", "B2", "outro"], loop_to=1)


# ---------------------------------------------------------------------------
# Depth 2: "Mossy Halls" -- E minor, lush and green.
# ---------------------------------------------------------------------------
def depth2():
    prog_a = "Em:4 C:4 G:4 D:4 Em:4 C:4 Am:4 B:4"
    tune_a = ("B4/4 E5/4 G5/4. F#5/8 | E5/4 D5/8 E5/8 C5/2 | D5/4 G5/4 B5/4. A5/8 | F#5/2 r/4 D5/4 | "
              "E5/4 G5/8 F#5/8 E5/4 B4/4 | C5/4 E5/4 G5/4 E5/4 | A5/4. G5/8 F#5/4 E5/4 | D#5/2 F#5/2")
    prog_b = "C:4 D:4 Bm:4 Em:4 Am:4 D:4 G:4 B7:4"
    tune_b = "E5/2 G5/2 | F#5/2 A5/2 | B5/2. A5/4 | G5/1 | E5/2 A5/2 | F#5/2 D5/2 | G5/4 F#5/4 E5/4 D5/4 | D#5/1"

    def bed(sec):
        sec.arp("0 1 2 1 2 1 0 1", "harp", 22, low="E3")
        sec.pad(vel=20)

    def brushes(sec):
        sec.drums(CH_HIGH, "shaker", EIGHTHS, vel=10)
        sec.drums(CH_LOW, "kick", "x.........x.....", vel=24)

    intro = Section("intro", 4, "Em:4 C:4 Em:4 C:4")
    bed(intro)
    intro.sustain(CH_TEXTURE, "choir", 18, "G4", degree=0)

    a = Section("A", 8, prog_a)
    a.melody(CH_LEAD, "epiano", 44, tune_a)
    bed(a)
    a.bass("R.......5...R...", vel=38)
    brushes(a)
    a.drums(CH_COUNTER, "rim", BACKBEAT, vel=14)

    b = Section("B", 8, prog_b)
    b.melody(CH_LEAD, "strings", 38, tune_b)
    b.arp("0 1 2 3", "epiano", 22, low="C5", ch=CH_COUNTER, rows_per_step=4)
    bed(b)
    b.bass("R.......5.......", vel=38)
    brushes(b)

    a2 = Section("A2", 8, prog_a)
    a2.melody(CH_LEAD, "flute", 42, tune_a, transpose=12)
    a2.arp("0 2 1 2", "epiano", 22, low="E4", ch=CH_COUNTER, rows_per_step=4)
    bed(a2)
    a2.bass("R.......5...R...", vel=38)
    brushes(a2)

    c = Section("C", 8, "Cmaj7:4 G:4 Cmaj7:4 D:4 Am:4 Em:4 F:4 B:4")
    c.melody(CH_LEAD, "choir", 36, "E5/1 | D5/1 | E5/2 G5/2 | F#5/1 | E5/2 C5/2 | B4/2 G4/2 | A4/2 C5/2 | B4/1")
    c.arp("0 1 2 3 2 1 2 1", "harp", 22, low="C3")
    c.pad(vel=22)
    c.bass("R" + "." * 15, vel=36)
    c.drums(CH_HIGH, "shaker", OFFBEATS, vel=10)

    outro = Section("outro", 4, "Em:4 C:4 Am:4 B:4")
    outro.melody(CH_LEAD, "epiano", 40, "B4/4 E5/4 G5/4 B5/4 | C6/2 G5/2 | A5/2 E5/2 | D#5/1")
    bed(outro)
    outro.bass("R" + "." * 15, vel=34)
    return Song("depth2", 84, [intro, a, b, a2, c, outro],
                ["intro", "A", "B", "A2", "C", "B", "outro"], loop_to=1)


# ---------------------------------------------------------------------------
# Depth 3: "Rust Caverns" -- C# Phrygian, sparse and ominous.
# ---------------------------------------------------------------------------
def depth3():
    def cello_ostinato(sec, first_bar, roots):
        """Two-bar figures in eighths over each root: R . R 5 R . b2 R."""
        text = []
        for root in roots:
            r, f, s = root
            text.append("%s/8 r/8 %s/8 %s/8 %s/8 r/8 %s/8 %s/8" % (r, r, f, r, s, r))
        sec.melody(CH_BASS, "cello", 34, " | ".join(text), bar=first_bar, legato=0.8)

    cs = ("C#2", "G#2", "D2")
    d = ("D2", "A2", "Eb2")
    b_ = ("B1", "F#2", "C2")
    a_ = ("A1", "E2", "Bb1")

    intro = Section("intro", 4, "C#5:8 D5:8")
    intro.pad(vel=22)
    intro.note(CH_LOW, 0, 45, "gong", 26)
    intro.sustain(CH_TEXTURE, "choir", 18, "G#3", degree=1)

    a = Section("A", 8, "C#5:8 D5:8 C#5:8 B5:4 D5:4")
    a.pad(vel=20)
    cello_ostinato(a, 0, [cs, cs, d, d, cs, cs, b_, d])
    a.melody(CH_LEAD, "horn_soft", 36, "r/1 | C#4/2 D4/2 | E4/1 | r/1 | C#4/2 B3/4 C#4/4 | D4/1 | r/1 | D4/2 C#4/2")
    a.melody(CH_COUNTER, "celesta", 26, "G#5/8 A5/8 G#5/4 r/2 | r/1 | r/1 | A5/8 Bb5/8 A5/4 r/2 | "
                                        "G#5/8 A5/8 G#5/4 r/2 | r/1 | r/1 | F#5/8 G5/8 F#5/4 r/2")
    a.drums(CH_HIGH, "rim", "x...x...x...x...", vel=12)
    a.drums(CH_LOW, "timpani", "x...............", vel=28, note="C#2")

    b = Section("B", 8, "A5:8 Bb5:8 C#5:8 D5:8")
    b.pad(vel=22)
    cello_ostinato(b, 0, [a_, a_, ("Bb1", "F2", "B1"), ("Bb1", "F2", "B1"), cs, cs, d, d])
    b.melody(CH_LEAD, "choir", 34, "E4/1 | F4/2 E4/2 | F4/1 | D4/2 F4/2 | E4/1 | G#4/2 E4/2 | F#4/1 | A4/2 F#4/2")
    b.drums(CH_HIGH, "rim", "x...x...x...x...", vel=12)
    b.note(CH_LOW, 0, 45, "gong", 22).note(CH_LOW, 64, 45, "gong", 22)

    a2 = Section("A2", 8, "C#5:8 D5:8 C#5:8 B5:4 D5:4")
    a2.pad(vel=20)
    cello_ostinato(a2, 0, [cs, cs, d, d, cs, cs, b_, d])
    a2.melody(CH_LEAD, "flute", 34, "r/1 | C#5/2 D5/2 | E5/2. D5/4 | C#5/1 | G#5/2 A5/4 G#5/4 | "
                                    "F#5/2 E5/2 | D5/2 E5/4 F#5/4 | D5/1")
    a2.melody(CH_COUNTER, "celesta", 24, "G#5/8 A5/8 G#5/4 r/2 | r/1 | r/1 | A5/8 Bb5/8 A5/4 r/2 | "
                                         "G#5/8 A5/8 G#5/4 r/2 | r/1 | r/1 | F#5/8 G5/8 F#5/4 r/2")
    a2.drums(CH_HIGH, "rim", "x...x...x...x.x.", vel=12)
    a2.drums(CH_LOW, "timpani", "x.......x.......", vel=26, note="C#2")

    c = Section("C", 8, "C#5:8 E5:8 D5:8 C#5:8")
    c.pad(vel=24)
    cello_ostinato(c, 0, [cs, cs, ("E2", "B2", "F2"), ("E2", "B2", "F2"), d, d, cs, cs])
    c.melody(CH_LEAD, "horn", 40, "C#4/2 D4/2 | E4/1 | G#4/2 A4/2 | B4/1 | A4/2 F#4/4 E4/4 | "
                                  "E4/2 D4/2 | C#4/1+1")
    c.drums(CH_LOW, "timpani", "x...x...x...x...", vel=26, note="C#2", bars=range(4))
    c.drums(CH_LOW, "timpani", "x.x.x.x.x.x.xxxx", vel=30, note="C#2", bars=range(4, 8))
    c.drums(CH_HIGH, "rim", EIGHTHS, vel=12)

    outro = Section("outro", 4, "C#5:8 D5:8")
    outro.pad(vel=22)
    outro.sustain(CH_TEXTURE, "choir", 20, "G#3", degree=1)
    outro.note(CH_LOW, 0, 45, "gong", 24)
    outro.melody(CH_COUNTER, "celesta", 22, "G#5/8 A5/8 G#5/4 r/2 | r/1 | A5/8 Bb5/8 A5/4 r/2 | r/1")
    return Song("depth3", 70, [intro, a, b, a2, c, outro],
                ["intro", "A", "B", "A2", "C", "outro"], loop_to=1)


# ---------------------------------------------------------------------------
# Depth 4: "Halls of Black Brick" -- C minor, a tense ostinato.
# ---------------------------------------------------------------------------
def depth4():
    prog_a = "Cm:4 Ab:4 Bb:4 G:4 Cm:4 Ab:4 Fm:4 G:4"
    tune_a = ("G4/4. Ab4/8 G4/4 Eb4/4 | F4/4. G4/8 Ab4/4 C5/4 | Bb4/2 D5/4 F5/4 | D5/1 | "
              "G4/4. Ab4/8 G4/4 Eb4/4 | C5/4. Bb4/8 Ab4/4 C5/4 | F5/4 Eb5/4 D5/4 C5/4 | B4/1")
    prog_b = "Ab:4 Eb:4 Bb:4 Cm:4 Fm:4 Cm:4 D:4 G:4"
    tune_b = "C5/2 Eb5/2 | Bb4/2 G4/2 | D5/2 F5/2 | Eb5/1 | F5/2 Ab5/2 | G5/2 Eb5/2 | F#5/2 A5/2 | G5/1"

    def engine(sec, drums=True):
        sec.arp("0 1 2 1", "strings_fast", 28, low="C4")
        sec.bass("R.R.R.R.R.R.R.R.", vel=40)
        sec.pad(vel=18)
        if drums:
            sec.drums(CH_LOW, "kick", "x.......x.x.....", vel=34)
            sec.drums(CH_HIGH, "snare", BACKBEAT, vel=30)

    intro = Section("intro", 4, "Cm:4 Cm:4 Ab:4 G:4")
    intro.arp("0 1 2 1", "strings_fast", 28, low="C4")
    intro.pad(vel=18)
    intro.drums(CH_LOW, "timpani", "x...............", vel=32, note="C2")
    intro.drums(CH_HIGH, "snare", "........x.x.xxxx", vel=24, bars=[3])

    a = Section("A", 8, prog_a)
    a.melody(CH_LEAD, "horn", 42, tune_a)
    engine(a)

    b = Section("B", 8, prog_b)
    b.melody(CH_LEAD, "strings", 38, tune_b)
    b.sustain(CH_TEXTURE, "choir", 20, "Eb4", degree=1)
    engine(b)

    a2 = Section("A2", 8, prog_a)
    a2.melody(CH_LEAD, "horn", 44, tune_a)
    a2.melody(CH_COUNTER, "celesta", 24, "r/2 Eb6/4 C6/4 | r/2 Ab5/4 C6/4 | r/2 F6/4 D6/4 | r/2 B5/4 D6/4 | "
                                         "r/2 Eb6/4 C6/4 | r/2 Ab5/4 C6/4 | r/2 Ab5/4 F5/4 | r/2 D6/4 B5/4")
    engine(a2)

    c = Section("C", 8, "Fm:4 Ab:4 Eb:4 Bb:4 Fm:4 Ab:4 G:8")
    c.melody(CH_LEAD, "epiano", 40, "C5/4 Ab4/4 F4/4 Ab4/4 | Eb5/2 C5/2 | Bb4/4 G4/4 Eb4/4 G4/4 | D5/2 Bb4/2 | "
                                    "C5/4 Ab4/4 F4/4 Ab4/4 | Eb5/2 F5/2 | G5/1+1")
    c.pad(vel=22)
    c.sustain(CH_TEXTURE, "strings", 22, "C4")
    c.bass("R" + "." * 15, vel=36)
    c.drums(CH_LOW, "timpani", "x.......x.......", vel=28, note="F2", bars=range(6))
    c.drums(CH_LOW, "timpani", "x.x.x.x.xxxxXXXX", vel=28, note="G2", bars=[6, 7])

    outro = Section("outro", 4, "Cm:4 Ab:4 Fm:4 G:4")
    outro.melody(CH_LEAD, "horn", 40, "G4/4. Ab4/8 G4/4 Eb4/4 | C5/2 Ab4/2 | F4/4 Ab4/4 C5/4 Eb5/4 | D5/1")
    engine(outro)
    outro.drums(CH_HIGH, "snare", "........x.x.xxxx", vel=26, bars=[3])
    return Song("depth4", 96, [intro, a, b, a2, c, outro],
                ["intro", "A", "B", "A2", "C", "B", "outro"], loop_to=1)


# ---------------------------------------------------------------------------
# Depth 5: "Crystal Caverns" -- D Lydian, shimmering.
# ---------------------------------------------------------------------------
def depth5():
    prog_a = "D:4 E:4 D:4 E:4 Bm:4 G:4 A:4 E:4"
    tune_a = ("A5/4 F#5/8 E5/8 D5/4 E5/4 | G#5/2 F#5/4 E5/4 | F#5/4 A5/4 D6/4 C#6/8 B5/8 | B5/2 G#5/2 | "
              "F#5/4 D5/4 B4/4 D5/4 | G5/4 B5/4 D6/2 | C#6/4 B5/4 A5/4 E5/4 | G#5/1")
    prog_b = "G:4 A:4 F#m:4 Bm:4 G:4 A:4 D:4 E:4"
    tune_b = "B5/2 G5/2 | A5/2 E5/2 | F#5/4 A5/4 C#6/2 | B5/1 | D6/2 B5/2 | C#6/2 A5/2 | F#5/4 A5/4 D6/4 F#6/4 | E6/1"

    def sparkle(sec, vel=20):
        sec.arp("0 1 2 3 4 3 2 1", "musicbox", vel, low="D5", ch=CH_COUNTER)

    def bed(sec):
        sec.arp("0 1 2 1", "harp", 22, low="D3")
        sec.pad(vel=20)

    intro = Section("intro", 4, "D:4 E:4 D:4 E:4")
    sparkle(intro)
    intro.pad(vel=20)
    intro.sustain(CH_TEXTURE, "choir", 18, "F#4", degree=1)

    a = Section("A", 8, prog_a)
    a.melody(CH_LEAD, "celesta", 42, tune_a)
    bed(a)
    sparkle(a, 16)
    a.bass("R.......5.......", vel=34)
    a.drums(CH_HIGH, "shaker", OFFBEATS, vel=12)

    b = Section("B", 8, prog_b)
    b.melody(CH_LEAD, "flute", 40, tune_b)
    b.sustain(CH_TEXTURE, "choir", 20, "D4", degree=1)
    bed(b)
    b.bass("R.......5.......", vel=34)
    b.drums(CH_HIGH, "shaker", OFFBEATS, vel=12)

    a2 = Section("A2", 8, prog_a)
    a2.melody(CH_LEAD, "celesta", 42, tune_a)
    a2.sustain(CH_TEXTURE, "strings", 24, "A4", degree=1)
    bed(a2)
    sparkle(a2, 16)
    a2.bass("R.......5.......", vel=34)
    a2.drums(CH_HIGH, "shaker", EIGHTHS, vel=12)

    c = Section("C", 8, "Bm:4 G:4 D:4 A:4 Bm:4 G:4 E:8")
    c.melody(CH_LEAD, "epiano", 40, "F#5/4 D5/4 B4/2 | G5/4 D5/4 B4/2 | A5/4 F#5/4 D5/2 | E5/4 C#5/4 A4/2 | "
                                    "F#5/4 D5/4 B4/4 D5/4 | G5/2 B5/2 | G#5/1+1")
    c.arp("0 1 2 3 2 1 2 1", "harp", 22, low="B2")
    c.pad(vel=22)
    c.sustain(CH_TEXTURE, "choir", 20, "D4", degree=2)
    c.bass("R" + "." * 15, vel=32)

    outro = Section("outro", 4, "D:4 E:4 D:4 E:4")
    sparkle(outro, 18)
    outro.pad(vel=20)
    outro.melody(CH_LEAD, "celesta", 36, "A5/4 F#5/4 D5/2 | G#5/1 | A5/4 D6/4 F#6/2 | E6/1")
    return Song("depth5", 84, [intro, a, b, a2, c, outro],
                ["intro", "A", "B", "A2", "C", "B", "outro"], loop_to=1)


# ---------------------------------------------------------------------------
# Depth 6: "The Rune Vaults" -- B minor, slow and eerie.
# ---------------------------------------------------------------------------
def depth6():
    prog_a = "Bm:8 G:8 Bm:8 F#:8"
    tune_a = ("F#5/2 B5/4 A5/4 | F5/4. E5/8 D5/2 | D5/4 E5/4 G5/2 | B5/1 | "
              "F#5/2 D6/4 C#6/4 | B5/2. A5/4 | A#5/4 G#5/4 F#5/4 E5/4 | F#5/1")

    def heartbeat(sec, bars=None):
        sec.drums(CH_LOW, "kick", "x..x............", vel=22, bars=bars)
        sec.drums(CH_HIGH, "rim", "x.......x.......", vel=10, bars=bars)

    intro = Section("intro", 4, "Bm:8 G5:8")
    intro.sustain(CH_TEXTURE, "choir", 22, "B3")
    intro.pad(vel=20)
    intro.note(CH_LOW, 0, 45, "gong", 24)
    heartbeat(intro, bars=[2, 3])

    a = Section("A", 8, prog_a)
    a.melody(CH_LEAD, "flute", 36, tune_a)
    a.melody(CH_COUNTER, "musicbox", 18, "r/2 B6/4 r/4 | r/1 | r/2 D7/4 r/4 | r/1 | r/2 F#6/4 r/4 | r/1 | "
                                         "r/2 C#7/4 r/4 | r/1")
    a.pad(vel=20)
    a.sustain(CH_TEXTURE, "choir", 18, "B3")
    heartbeat(a)

    b = Section("B", 8, "Em:8 Bm:8 C:8 F#:8")
    b.melody(CH_LEAD, "choir", 32, "E4/1 | G4/1 | F#4/1 | D4/1 | E4/1 | G4/2 E4/2 | F#4/1 | A#4/1")
    b.arp("0 1 2 1", "epiano", 20, low="E3", ch=CH_ARP, rows_per_step=4)
    b.pad(vel=20)
    b.note(CH_LOW, 0, 45, "gong", 22)
    b.drums(CH_HIGH, "rim", "x.......x.......", vel=10)

    a2 = Section("A2", 8, prog_a)
    a2.melody(CH_LEAD, "celesta", 38, tune_a)
    a2.melody(CH_COUNTER, "flute", 26, "r/1 | B4/2 A4/2 | r/1 | D5/2 E5/2 | r/1 | F#5/2 E5/2 | r/1 | C#5/1")
    a2.pad(vel=20)
    a2.sustain(CH_TEXTURE, "strings", 20, "D4", degree=1)
    heartbeat(a2)

    c = Section("C", 8, "Gmaj7:8 F#m:8 Em:8 F#:8")
    c.melody(CH_LEAD, "strings", 34, "D5/1 | B4/1 | C#5/1 | A4/1 | B4/1 | G4/1 | A#4/1 | F#4/1")
    c.arp("0 1 2 3 2 1", "harp", 20, low="G2", rows_per_step=4)
    c.pad(vel=20)
    c.sustain(CH_TEXTURE, "choir", 18, "B3", degree=0)
    c.note(CH_LOW, 0, 45, "gong", 20)

    outro = Section("outro", 4, "Bm:8 F#:8")
    outro.sustain(CH_TEXTURE, "choir", 20, "B3")
    outro.pad(vel=20)
    outro.melody(CH_LEAD, "flute", 32, "F#5/2 B5/4 A5/4 | F5/2 E5/2 | D5/2 C#5/2 | F#5/1")
    outro.note(CH_LOW, 32, 45, "gong", 24)
    return Song("depth6", 66, [intro, a, b, a2, c, outro],
                ["intro", "A", "B", "A2", "C", "outro"], loop_to=1)


# ---------------------------------------------------------------------------
# Depth 7: "The Long Descent" -- G minor, driving toward the Warden.
# ---------------------------------------------------------------------------
def depth7():
    prog_a = "Gm:4 Eb:4 Bb:4 F:4 Gm:4 Eb:4 Cm:4 D:4"
    tune_a = ("G4/4 Bb4/4 D5/4. C5/8 | Bb4/4 G4/4 Eb4/2 | F4/4 Bb4/4 D5/4 F5/4 | C5/2. r/4 | "
              "G4/4 Bb4/4 D5/4. Eb5/8 | Eb5/4 C5/4 Bb4/2 | A4/4 Bb4/4 C5/4 Eb5/4 | D5/1")
    prog_b = "Eb:4 Bb:4 Cm:4 Gm:4 Eb:4 F:4 D:8"
    tune_b = "G5/2 Eb5/2 | F5/2 D5/2 | Eb5/2 G5/2 | D5/1 | G5/2 Bb5/2 | A5/2 F5/2 | F#5/1+1"

    def drive(sec, kick="x...x...x...x..."):
        sec.bass("R.R.R.R.R.R.O.R.", vel=42)
        sec.drums(CH_LOW, "kick", kick, vel=34)
        sec.drums(CH_HIGH, "snare", BACKBEAT, vel=30)
        sec.arp("0 1 2 1", "strings_fast", 26, low="G3")
        sec.pad(vel=18)

    intro = Section("intro", 4, "Gm:4 Gm:4 Eb:4 F:4")
    intro.arp("0 1 2 1", "strings_fast", 26, low="G3")
    intro.bass("R.R.R.R.R.R.R.R.", vel=38)
    intro.drums(CH_LOW, "kick", "x...x...x...x...", vel=30, bars=[2, 3])
    intro.drums(CH_HIGH, "snare", "........x.x.xxxx", vel=24, bars=[3])

    a = Section("A", 8, prog_a)
    a.melody(CH_LEAD, "horn", 42, tune_a)
    drive(a)

    b = Section("B", 8, prog_b)
    b.melody(CH_LEAD, "strings", 38, tune_b)
    b.sustain(CH_TEXTURE, "choir", 20, "Bb3", degree=1)
    b.bass("R.R.R.R.R.R.O.R.", vel=40)
    b.drums(CH_LOW, "kick", "x.......x.......", vel=32)
    b.drums(CH_HIGH, "snare", "........x.......", vel=30)
    b.arp("0 1 2 1", "strings_fast", 24, low="G3")
    b.pad(vel=18)

    a2 = Section("A2", 8, prog_a)
    a2.melody(CH_LEAD, "horn", 44, tune_a, transpose=12)
    a2.melody(CH_COUNTER, "celesta", 24, "r/2 D6/4 Bb5/4 | r/2 G5/4 Bb5/4 | r/2 F6/4 D6/4 | r/2 C6/4 A5/4 | "
                                         "r/2 D6/4 Bb5/4 | r/2 G5/4 Bb5/4 | r/2 Eb6/4 C6/4 | r/2 F#5/4 A5/4")
    drive(a2, kick="x...x...x...x.x.")

    c = Section("C", 8, "Cm:4 Gm:4 Bb:4 F:4 Cm:4 Gm:4 D:8")
    c.melody(CH_LEAD, "epiano", 40, "Eb5/4 C5/4 G4/4 C5/4 | D5/4 Bb4/4 G4/2 | F5/4 D5/4 Bb4/4 D5/4 | C5/2 A4/2 | "
                                    "Eb5/4 G5/4 C6/4 G5/4 | Bb5/4 G5/4 D5/2 | F#5/1+1")
    c.pad(vel=22)
    c.sustain(CH_TEXTURE, "strings", 22, "D4", degree=1)
    c.bass("R" + "." * 15, vel=36)
    c.drums(CH_LOW, "timpani", "x.......x.......", vel=28, note="G2", bars=range(6))
    c.drums(CH_LOW, "timpani", "x.x.x.x.xxxxXXXX", vel=28, note="D2", bars=[6, 7])

    outro = Section("outro", 4, "Gm:4 Eb:4 F:4 D:4")
    outro.melody(CH_LEAD, "horn", 40, "G4/4 Bb4/4 D5/2 | Eb5/2 Bb4/2 | C5/4 D5/4 F5/2 | F#5/1")
    drive(outro)
    return Song("depth7", 116, [intro, a, b, a2, c, outro],
                ["intro", "A", "B", "A2", "C", "B", "outro"], loop_to=1)


def build_songs():
    """All songs in MUSIC_* order (delve.h)."""
    return [title(), victory(), death(), boss(),
            depth1(), depth2(), depth3(), depth4(), depth5(), depth6(), depth7()]
