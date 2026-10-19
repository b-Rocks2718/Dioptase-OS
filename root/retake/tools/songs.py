"""Instruments and songs (notation in music.py). Silent pictures were played
live by a theatre pianist or organist, so the score is mostly piano, with
the organ for the haunted Backlot.

Instrument fields: (name, wave, peak level 0-15, decay ticks per level step
(0 = hold), sustain level, release ticks per level step, vibrato depth,
vibrato rate).
"""

INSTRUMENTS = [
    ("piano", "piano", 13, 7, 4, 2, 0, 0),
    ("piano_bass", "piano", 12, 9, 5, 2, 0, 0),
    ("piano_chord", "piano", 8, 5, 2, 1, 0, 0),
    ("organ", "organ", 9, 0, 9, 4, 0, 0),
    ("organ_lead", "reed", 10, 0, 10, 3, 2, 6),
    ("music_box", "sine", 11, 9, 0, 3, 0, 0),
    ("square_lead", "pulse", 10, 12, 6, 2, 1, 5),
    ("tri_bass", "triangle", 13, 0, 13, 2, 0, 0),
    ("tom", "triangle", 14, 2, 0, 1, 0, 0),
    ("marimba", "sine", 12, 4, 0, 2, 0, 0),
    ("theremin", "sine", 11, 0, 11, 3, 3, 4),
    ("kick", "triangle", 15, 1, 0, 1, 0, 0),
    ("snare", "noise", 10, 1, 0, 1, 0, 0),
    ("hat", "noise", 5, 1, 0, 1, 0, 0),
]

CHORDS = {
    "C": ("C3", "G2", "E4", "G4"),
    "C7": ("C3", "G2", "E4", "Bb4"),
    "A7": ("A2", "E2", "C#4", "G4"),
    "D7": ("D3", "A2", "F#4", "C5"),
    "Dm": ("D3", "A2", "F4", "A4"),
    "G7": ("G2", "D3", "F4", "B4"),
    "G": ("G2", "D3", "D4", "B4"),
    "D": ("D3", "A2", "F#4", "A4"),
    "F": ("F2", "C3", "A4", "C5"),
    "Fm": ("F2", "C3", "Ab4", "C5"),
    "F#dim": ("F#2", "C3", "A4", "Eb5"),
    "C/G": ("G2", "C3", "E4", "C5"),
    "Em": ("E2", "B2", "G4", "B4"),
    "B7": ("B2", "F#2", "D#4", "A4"),
    "E7": ("E2", "B2", "G#4", "D5"),
    "Am": ("A2", "E3", "C4", "E4"),
    "Dm/A": ("A2", "D3", "F4", "A4"),
    "Bb": ("Bb2", "F2", "D4", "F4"),
    "Gm": ("G2", "D3", "Bb4", "D5"),
    "Cm": ("C3", "G2", "Eb4", "G4"),
    "Ab": ("Ab2", "Eb3", "C4", "Eb4"),
}


def oompah(beats):
    """Stride left hand, one entry per beat (4 rows): the bass on the beat
    alternating root and fifth, a staccato two-note chord on the off-beat."""
    bass, c1, c2 = [], [], []
    for i, name in enumerate(beats):
        root, alt, t1, t2 = CHORDS[name]
        bass += [root if i % 2 == 0 else alt, "-", "-", "-"]
        c1 += ["r", "r", t1, "."]
        c2 += ["r", "r", t2, "."]
    return " ".join(bass), " ".join(c1), " ".join(c2)


def held_chords(beats, rows_per_beat):
    """Sustained organ chords: each beat entry holds for rows_per_beat."""
    lo, hi = [], []
    for name in beats:
        root, alt, t1, t2 = CHORDS[name]
        lo += [t1] + ["-"] * (rows_per_beat - 1)
        hi += [t2] + ["-"] * (rows_per_beat - 1)
    return " ".join(lo), " ".join(hi)


# ---- Stage 1: "Safety Last Street" (a rag for a silent comedy) ----
RAG_MELODY = """
E5 G5 - C6 | - G5 E5 - | C#5 E5 - A5 | - G5 E5 - | F#5 A5 - D6 | - C6 A5 - | B5 - A5 G5 | F5 - D5 B4 |
C5 E5 - G5 | C6 - B5 C6 | E5 - D5 C#5 | E5 - A5 G5 | F#5 - A5 F#5 | G5 - F5 D5 | C5 - E5 G5 | C6 - . r |
A5 C6 - F6 | - C6 A5 - | A5 C6 - Eb6 | - C6 A5 - | G5 C6 - E6 | - C6 G5 - | E5 G5 - C#6 | - A5 G5 - |
F#5 A5 - C6 | D6 - C6 A5 | B5 D6 - F6 | - D6 B5 G5 | E6 - D6 C6 | G5 - E5 C5 | D5 - F5 G5 | B5 - D6 - |
"""
RAG_BEATS = (["C", "C", "A7", "A7", "D7", "D7", "G7", "G7", "C", "C", "A7", "A7", "D7", "G7", "C", "C"] +
             ["F", "F", "F#dim", "F#dim", "C/G", "C/G", "A7", "A7", "D7", "D7", "G7", "G7", "C", "C", "G7", "G7"])
_rb, _r1, _r2 = oompah(RAG_BEATS)

# ---- Boss chase: a galop in A minor ----
CHASE_MELODY = """
A5 C6 E6 C6 | A5 C6 E6 C6 | B5 D6 E6 D6 | B5 G#5 E5 G#5 | A5 C6 E6 A6 | G#6 E6 B5 G#5 | A5 E5 C5 E5 | A5 - . r |
F5 A5 C6 A5 | F5 A5 C6 A5 | E5 G#5 B5 G#5 | E5 G#5 B5 D6 | C6 B5 A5 G#5 | A5 B5 C6 D6 | E6 D6 C6 B5 | A5 - E5 - |
"""
CHASE_BEATS = ["Am", "Am", "E7", "E7", "Am", "E7", "Am", "Am", "Dm/A", "Dm/A", "E7", "E7", "Am", "Dm/A", "E7", "Am"]
_cb, _c1, _c2 = oompah(CHASE_BEATS)

# ---- The Backlot: an organ alone in an empty studio ----
BACKLOT_LEAD = """
r - - - - - - - | D5 - - - F5 - - - | E5 - - - - - D5 - | C#5 - - - - - - - |
r - - - - - - - | F5 - - - A5 - - - | G5 - - - F5 - E5 - | D5 - - - - - - - |
r - - - - - - - | Bb4 - - - D5 - - - | C5 - - - Bb4 - A4 - | A4 - - - - - - - |
r - - - - - - - | G4 - - - Bb4 - - - | A4 - - - G4 - F4 - | E4 - - - - - . - |
"""
BACKLOT_BEATS = ["Dm", "Dm", "Dm", "Dm", "Gm", "Gm", "A7", "A7", "Dm", "Dm", "Dm", "Dm", "Bb", "Bb", "A7", "A7",
                 "Bb", "Bb", "Bb", "Bb", "F", "F", "A7", "A7", "Gm", "Gm", "Gm", "Gm", "A7", "A7", "A7", "A7"]
_bl1, _bl2 = held_chords(BACKLOT_BEATS, 4)
_blbass = " ".join([[CHORDS[b][0] for b in BACKLOT_BEATS][i] if j == 0 else "-" for i in range(len(BACKLOT_BEATS)) for j in range(4)])

# ---- Title overture: the rag's B strain, slower, on a music box ----
TITLE_MELODY = """
A5 C6 - F6 | - C6 A5 - | A5 C6 - Eb6 | - C6 A5 - | G5 C6 - E6 | - C6 G5 - | E5 G5 - C#6 | - A5 G5 - |
F#5 A5 - C6 | D6 - C6 A5 | B5 D6 - F6 | - D6 B5 G5 | E6 - D6 C6 | G5 - E5 C5 | C5 - - - | - - . r |
"""
TITLE_BEATS = ["F", "F", "F#dim", "F#dim", "C/G", "C/G", "A7", "A7", "D7", "D7", "G7", "G7", "C", "C", "C", "C"]
_tb, _t1, _t2 = oompah(TITLE_BEATS)

# ---- Stage 2: a trail tune for harmonica (reed) over a loping bass ----
WEST_MELODY = """
G4 - - B4 | D5 - - B4 | C5 - B4 A4 | G4 - - - | E4 - - G4 | A4 - G4 E4 | D4 - - - | . - - - |
G4 - - B4 | D5 - - G5 | F#5 - E5 D5 | E5 - - - | C5 - - E5 | D5 - B4 G4 | A4 - - - | . - - - |
G4 - - B4 | D5 - - B4 | C5 - B4 A4 | G4 - - - | E4 - - G4 | A4 - G4 E4 | D4 - F#4 A4 | G4 - - - |
"""
WEST_BEATS = ["G", "G", "C", "G", "C", "C", "D7", "D7", "G", "G", "D7", "C", "C", "G", "D7", "D7",
              "G", "G", "C", "G", "C", "C", "D7", "G"]
_wb, _w1, _w2 = oompah(WEST_BEATS)

# ---- Stage 3: a sea shanty for the squeezebox ----
SHANTY_MELODY = """
D5 - D5 - | D5 - D5 D5 | D5 - A4 - | F4 - A4 - | C5 - C5 - | C5 - C5 C5 | C5 - G4 - | E4 - G4 - |
D5 - D5 - | D5 - D5 D5 | D5 - E5 - | F5 - G5 - | F5 - D5 - | C5 - A4 - | D5 - - - | . - - - |
F5 - F5 - | E5 - E5 - | D5 - C5 - | A4 - - - | C5 - C5 - | D5 - E5 - | F5 - E5 - | D5 - - - |
"""
SHANTY_BEATS = ["Dm", "Dm", "Dm", "Dm", "C", "C", "C", "C", "Dm", "Dm", "Dm", "Dm", "C", "C", "Dm", "Dm",
                "Dm", "C", "Dm", "Dm", "C", "C", "Dm", "Dm"]
_sb, _s1, _s2 = oompah(SHANTY_BEATS)

# ---- Stage 4: drums and marimba in the jungle ----
JUNGLE_MELODY = """
A4 - C5 - | E5 - D5 C5 | A4 - - - | G4 - A4 - | C5 - D5 - | E5 - G5 E5 | D5 - - - | . - - - |
A4 - C5 - | E5 - D5 C5 | A4 - - - | G4 - E4 - | G4 - A4 - | C5 - D5 C5 | A4 - - - | . - - - |
"""
JUNGLE_TOMS = " ".join(["A2 - A2 - E2 - A2 -"] * 8)
JUNGLE_BASS = " ".join(["A2 - - - - - - -", "G2 - - - - - - -", "A2 - - - - - - -", "E2 - - - - - - -"] * 2)

# ---- Stage 5: an organ and a theremin in the haunted manor ----
HORROR_LEAD = """
E4 - - - - - - - | G4 - - - F#4 - - - | E4 - - - - - - - | B3 - - - - - . - |
C4 - - - - - - - | E4 - - - D#4 - - - | B3 - - - - - - - | . - - - - - - - |
E5 - - - D5 - - - | C5 - - - B4 - - - | A4 - - - C5 - - - | B4 - - - - - . - |
"""
HORROR_BEATS = ["Em", "Em", "Em", "Em", "Em", "Em", "B7", "B7", "C", "C", "Am", "Am", "B7", "B7", "B7", "B7",
                "Em", "Em", "C", "C", "Am", "Am", "B7", "B7"]
_hl1, _hl2 = held_chords(HORROR_BEATS, 4)
_hbass = " ".join([[CHORDS[b][0] for b in HORROR_BEATS][i] if j == 0 else "-"
                   for i in range(len(HORROR_BEATS)) for j in range(4)])

# ---- Stage 6: "Rocket Rangers of the Moon" (a serial's heroic march) ----
SPACE_MELODY = """
A4 - - - E5 - - - | A5 - G5 - E5 - C5 - | F5 - - - A5 - - - | G5 - F5 - E5 - D5 - |
D5 - - - G5 - - - | B5 - A5 - G5 - D5 - | E5 - - - G#5 - - - | B5 - - - - - . r |
A4 - - - E5 - - - | A5 - C6 - B5 - A5 - | C6 - - - A5 - F5 - | A5 - - - C6 - - - |
D6 - - - A5 - F5 - | D5 - F5 - A5 - D6 - | E6 - - - D6 - B5 - | G#5 - - - E5 - . r |
"""
SPACE_BEATS = ["Am", "Am", "F", "F", "G", "G", "E7", "E7", "Am", "Am", "F", "F", "Dm", "Dm", "E7", "E7"]
_sp_arp = " ".join(" ".join([CHORDS[b][2], ".", CHORDS[b][3], ".", CHORDS[b][2], ".", CHORDS[b][3], "."])
                   for b in SPACE_BEATS)
_sp_bass = " ".join(" ".join([CHORDS[b][0], "-", "-", "-", CHORDS[b][1], "-", "-", "-"]) for b in SPACE_BEATS)

# ---- Stage 7: "Night City" (a slow, smoky blues in C minor) ----
NOIR_MELODY = """
G4 - - - Eb5 - D5 - | C5 - - - - - . r | Ab4 - - - F5 - Eb5 - | C5 - - - - - . r |
Eb5 - - - C5 - Ab4 - | B4 - - - D5 - F5 - | Eb5 - - - D5 - C5 - | G4 - - - - - . r |
G5 - - - Eb5 - G5 - | F5 - - - D5 - Bb4 - | Eb5 - - - C5 - Ab4 - | D5 - - - B4 - G4 - |
Ab4 - C5 - F5 - Ab5 - | G5 - - - F5 - D5 - | Eb5 - - - C5 - - - | - - - - - - . r |
"""
NOIR_BEATS = ["Cm", "Cm", "Fm", "Fm", "Ab", "G7", "Cm", "G7", "Cm", "Bb", "Ab", "G7", "Fm", "G7", "Cm", "Cm"]
_nb = " ".join(" ".join([CHORDS[b][0], "-", CHORDS[b][1], "-", CHORDS[b][0], "-", CHORDS[b][1], "-"]) for b in NOIR_BEATS)
_n1 = " ".join(" ".join(["r", "r", CHORDS[b][2], ".", "r", "r", CHORDS[b][2], "."]) for b in NOIR_BEATS)
_n2 = " ".join(" ".join(["r", "r", CHORDS[b][3], ".", "r", "r", CHORDS[b][3], "."]) for b in NOIR_BEATS)

# ---- Stage 8: "Starlight Revue" (a showstopper with a stride left hand) ----
SHOW_MELODY = """
E5 - G5 - C6 - - - | A5 - G5 - E5 - C5 - | D5 - F5 - A5 - - - | G5 - F5 - D5 - B4 - |
C5 - E5 - G5 - C6 - | C#6 - - - A5 - G5 - | F5 - - - A5 - D6 - | B5 - - - - - . r |
A5 - - - C6 - A5 - | Ab5 - - - C6 - Ab5 - | G5 - E5 - C5 - E5 - | G5 - - - E5 - C#5 - |
D5 - F#5 - A5 - C6 - | B5 - - - G5 - F5 - | E5 - G5 - C6 - - - | - - - - - - . r |
"""
SHOW_BEATS = [b for b in ["C", "Am", "Dm", "G7", "C", "A7", "Dm", "G7",
                          "F", "Fm", "C", "A7", "D7", "G7", "C", "C"] for _ in range(2)]
_sb, _s1, _s2 = oompah(SHOW_BEATS)

SONGS = {
    "title": {"tempo": 17, "channels": [("music_box", TITLE_MELODY), ("piano_bass", _tb),
                                        ("piano_chord", _t1), ("piano_chord", _t2)]},
    "stage1": {"tempo": 12, "channels": [("piano", RAG_MELODY), ("piano_bass", _rb),
                                         ("piano_chord", _r1), ("piano_chord", _r2)]},
    "chase": {"tempo": 8, "channels": [("piano", CHASE_MELODY), ("piano_bass", _cb),
                                       ("piano_chord", _c1), ("piano_chord", _c2)]},
    "stage2": {"tempo": 13, "channels": [("organ_lead", WEST_MELODY), ("tri_bass", _wb),
                                         ("piano_chord", _w1), ("piano_chord", _w2)]},
    "stage3": {"tempo": 11, "channels": [("organ_lead", SHANTY_MELODY), ("tri_bass", _sb),
                                         ("piano_chord", _s1), ("piano_chord", _s2)]},
    "stage4": {"tempo": 10, "channels": [("marimba", JUNGLE_MELODY), ("tom", JUNGLE_TOMS),
                                         ("tri_bass", JUNGLE_BASS)]},
    "stage5": {"tempo": 15, "channels": [("theremin", HORROR_LEAD), ("organ", _hl1), ("organ", _hl2),
                                         ("tri_bass", _hbass)]},
    "stage6": {"tempo": 11, "channels": [("square_lead", SPACE_MELODY), ("marimba", _sp_arp),
                                         ("tri_bass", _sp_bass)]},
    "stage7": {"tempo": 14, "channels": [("organ_lead", NOIR_MELODY), ("piano_chord", _n1),
                                         ("piano_chord", _n2), ("tri_bass", _nb)]},
    "stage8": {"tempo": 15, "channels": [("piano", SHOW_MELODY), ("piano_bass", _sb),
                                         ("piano_chord", _s1), ("piano_chord", _s2)]},
    "backlot": {"tempo": 16, "channels": [("organ_lead", BACKLOT_LEAD), ("organ", _bl1),
                                          ("organ", _bl2), ("tri_bass", _blbass)]},
}


# ---- Percussion ----
# Each stage song gets a kit that follows its own bass line: bass onsets
# alternate kick and snare (the stride "boom-chick"), and a hi-hat ticks on
# every other row. Styles pick the parts; a song keeps at most six channels.
KICK, SNARE, HAT = "C2", "D6", "C8"
DRUM_STYLES = {
    "stage1": "ks", "chase": "ksh", "stage2": "ks", "stage3": "ks", "stage4": "h",
    "stage5": "k", "stage6": "ksh", "stage7": "h", "stage8": "ksh", "backlot": "h",
}


def _tokens(text):
    return text.replace("|", " ").split()


def drum_channels(song, style):
    """Kick, snare and hat channels aligned to the song's bass channel (the
    channel played by a bass instrument, else the last one)."""
    bass = None
    for inst, text in song["channels"]:
        if "bass" in inst:
            bass = text
    if bass is None:
        bass = song["channels"][-1][1]
    toks = _tokens(bass)
    n = len(toks)
    onsets = [i for i, t in enumerate(toks) if t not in ("-", ".", "r")]
    kick = ["-"] * n
    snare = ["-"] * n
    hat = ["-"] * n
    for k, i in enumerate(onsets):
        if k % 2 == 0 or "s" not in style:
            kick[i] = KICK
        else:
            snare[i] = SNARE
    for i in range(0, n, 2):
        hat[i] = HAT
    out = []
    if "k" in style:
        out.append(("kick", " ".join(kick)))
    if "s" in style:
        out.append(("snare", " ".join(snare)))
    if "h" in style:
        out.append(("hat", " ".join(hat)))
    return out


for _name, _style in DRUM_STYLES.items():
    _song = SONGS[_name]
    _room = 6 - len(_song["channels"])
    _song["channels"] = _song["channels"] + drum_channels(_song, _style)[:_room]
