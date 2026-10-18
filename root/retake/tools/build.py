"""Build RETAKE's asset pack (retake.pak) and its C index header (pak.h).

Run from root/retake: `python3 tools/build.py`. The pack is a flat
little-endian file that the game maps read-only (mmap of /retake/retake.pak),
so every table below is 4-byte aligned and addressed by byte offsets from the
start of the file.

Layout:
    header      HEADER_FIELDS words (named in pak.h as PAKH_*)
    region[i]   block_count, blocks_off, backdrop_w, backdrop_h, backdrop_off
    room[i]     region, wx, wy, w, h, blocks_off, col_off, entity_count,
                entities_off, spawn_x, spawn_y, name_off
    sprite[i]   frame_count, frames_off   (frames are 32x32 packed pixels)
    font[i]     height, glyphs_off, pixels_off; glyphs are 95 entries for
                ' '..'~' of (advance u8, width u8, pad u16, pixel offset u32);
                pixels are width*height bytes of ink level 0..3
    script[i]   card_count, then card_count offsets of NUL-terminated card
                strings whose lines (already wrapped) end in '\n'
    hud tiles   8x8 packed tiles for the tile layer, uploaded from index 0
    card frame  320x236 packed image behind intertitle cards
    image[i]    w, h, pixels_off: entity art (tools/entart.py), packed pixels
Room block grids are u16 indices into the region's block table (64 pixels per
block); collision grids are u8 COL_* codes; entities are 8 shorts each:
type, tx, ty, a, b, c, d, e.
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
import pix  # noqa: E402
import region1  # noqa: E402
import rooms  # noqa: E402
import sprites  # noqa: E402
import text  # noqa: E402
import ui  # noqa: E402
import entart  # noqa: E402
import entities  # noqa: E402
import music  # noqa: E402
import songs  # noqa: E402
import font_serif  # noqa: E402
import font_title  # noqa: E402
from font import GLYPHS  # noqa: E402

MAGIC = 0x314B5452  # 'RTK1' little-endian
VERSION = 2
HEADER_FIELDS = ["magic", "version", "size", "region_count", "region_off", "room_count", "room_off",
                 "sprite_count", "sprite_off", "font_count", "font_off", "script_count", "script_off",
                 "hud_tile_count", "hud_tiles_off", "card_off", "image_count", "image_off",
                 "wave_count", "wave_off", "inst_count", "inst_off", "song_count", "song_off", "note_off"]
CARD_TEXT_W = 268   # widest wrapped line on an intertitle card, in pixels
FONTS = [("serif", font_serif), ("title", font_title)]
T = 8



def pack_font(module):
    """Serialize a generated font module: glyph table + ink-level bytes."""
    table = b""
    pixels = bytearray()
    for code in range(32, 127):
        adv, rows = module.GLYPHS[chr(code)]
        width = max(len(r) for r in rows)
        off = len(pixels)
        for r in rows:
            r = r.ljust(width)
            pixels += bytes(" .+#".index(c) for c in r)
        table += struct.pack("<BBHI", adv, width, 0, off)
    return module.HEIGHT, table, bytes(pixels)


def text_width(module, line):
    return sum(module.GLYPHS.get(c, module.GLYPHS["?"])[0] for c in line)


def wrap_card(module, card, max_w):
    """Wrap a card's paragraphs to max_w pixels; blank lines separate
    paragraphs and survive as empty lines."""
    out = []
    for para in card.split("\n"):
        if para.strip() == "":
            out.append("")
            continue
        line = ""
        for word in para.split():
            trial = word if not line else line + " " + word
            if text_width(module, trial) <= max_w:
                line = trial
            else:
                if not line:
                    raise SystemExit(f"build.py: word {word!r} is wider than an intertitle card")
                out.append(line)
                line = word
        out.append(line)
    for line in out:
        for c in line:
            if not (32 <= ord(c) < 127):
                raise SystemExit(f"build.py: character {c!r} in card text has no glyph")
    return "\n".join(out) + "\n"


def resolve(value, room, script_index, room_index):
    """An entity parameter: an integer, @script, #room or an ability name."""
    where = f"room '{room.name}'"
    if value.startswith("@"):
        if value[1:] not in script_index:
            raise SystemExit(f"build.py: {where} references unknown script {value}")
        return script_index[value[1:]]
    if value.startswith("#"):
        if value[1:] not in room_index:
            raise SystemExit(f"build.py: {where} references unknown room {value}")
        return room_index[value[1:]]
    if value in entities.ABILITIES:
        return entities.ABILITIES[value]
    try:
        return int(value, 0)
    except ValueError:
        raise SystemExit(f"build.py: {where} has entity parameter {value!r}, expected a number, @script, #room or ability")


class Pack:
    def __init__(self):
        self.data = bytearray()

    def align(self):
        while len(self.data) % 4:
            self.data.append(0)

    def add(self, blob):
        self.align()
        off = len(self.data)
        self.data += blob
        return off


def blockify(img, table, index):
    """Split a composed room into 8x8 blocks, deduplicated through `index`
    (bytes -> block id) into `table`; return the room's u16 block grid."""
    h, w = img.shape
    grid = []
    for ty in range(h // T):
        for tx in range(w // T):
            block = img[ty * T:(ty + 1) * T, tx * T:(tx + 1) * T]
            key = block.tobytes()
            bid = index.get(key)
            if bid is None:
                bid = len(table)
                index[key] = bid
                table.append(block.copy())
            grid.append(bid)
    return grid


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    room_files = sorted(
        os.path.join(root, "rooms", f) for f in os.listdir(os.path.join(root, "rooms")) if f.endswith(".txt")
    )
    all_rooms = []
    for path in room_files:
        all_rooms.extend(rooms.parse(path))
    names = [r.name for r in all_rooms]
    if len(set(names)) != len(names):
        raise SystemExit("build.py: duplicate room names in rooms/*.txt")

    regions = {1: region1.build(GLYPHS)}
    region_ids = sorted(regions)
    block_tables = {r: [] for r in region_ids}
    block_index = {r: {} for r in region_ids}
    room_grids = []
    for room in all_rooms:
        if room.region not in regions:
            raise SystemExit(f"build.py: room '{room.name}' uses region {room.region}, which has no art")
        tiles, stamps, _ = regions[room.region]
        img = rooms.compose(room, tiles, stamps)
        room_grids.append(blockify(img, block_tables[room.region], block_index[room.region]))

    sheets = sprites.build()

    pack = Pack()
    pack.data += bytes(4 * len(HEADER_FIELDS))

    region_entries = []
    for r in region_ids:
        blocks = np.concatenate([b.reshape(-1) for b in block_tables[r]]) if block_tables[r] else np.zeros(0)
        boff = pack.add(pix.le16(blocks))
        back = regions[r][2]
        doff = pack.add(pix.le16(back.reshape(-1)))
        region_entries.append((len(block_tables[r]), boff, back.shape[1], back.shape[0], doff))

    script_index = {name: i for i, name in enumerate(text.SCRIPTS)}
    room_index = {r.name: i for i, r in enumerate(all_rooms)}
    next_flag = [0]
    room_entries = []
    for room, grid in zip(all_rooms, room_grids):
        goff = pack.add(pix.le16(grid))
        col = bytes(c for row in room.col() for c in row)
        coff = pack.add(col)
        ents = b""
        for etype, tx, ty, props in room.entities:
            if etype not in entities.KINDS:
                raise SystemExit(f"build.py: room '{room.name}' has unknown entity kind '{etype}'")
            code = entities.KINDS[etype]
            vals = [code, tx, ty]
            for k in ("a", "b", "c", "d"):
                vals.append(resolve(props.get(k, "0"), room, script_index, room_index))
            if etype in entities.PERSISTENT:
                vals.append(next_flag[0])
                next_flag[0] += 1
            else:
                vals.append(-1)
            ents += struct.pack("<8h", *vals)
        eoff = pack.add(ents)
        sp = room.spawn() or (1, 1)
        noff = pack.add(room.name.encode() + b"\0")
        room_entries.append((room.region, room.wx, room.wy, room.w, room.h, goff, coff,
                             len(room.entities), eoff, sp[0], sp[1], noff))

    sprite_entries = []
    for name, frames in sheets:
        foff = pack.add(pix.le16(np.concatenate([f.reshape(-1) for f in frames])))
        sprite_entries.append((len(frames), foff))

    font_entries = []
    for name, module in FONTS:
        height, table, pixels = pack_font(module)
        goff = pack.add(table)
        poff = pack.add(pixels)
        font_entries.append((height, goff, poff))

    script_names = list(text.SCRIPTS)
    script_offs = []
    for name in script_names:
        cards = [wrap_card(font_serif, c, CARD_TEXT_W) for c in text.SCRIPTS[name]]
        card_offs = [pack.add(c.encode() + b"\0") for c in cards]
        script_offs.append(pack.add(struct.pack("<%dI" % (1 + len(cards)), len(cards), *card_offs)))

    hud, hud_index = ui.hud_tiles()
    hud_off = pack.add(pix.le16(np.concatenate([t.reshape(-1) for t in hud])))
    card_off = pack.add(pix.le16(ui.card_frame().reshape(-1)))

    wave_off = pack.add(b"".join(struct.pack("<256h", *music.wave_table(w)) for w in music.WAVES))
    inst_names = [i[0] for i in songs.INSTRUMENTS]
    inst_off = pack.add(b"".join(
        struct.pack("<8B", music.WAVES.index(wave), peak, decay, sustain, release, vd, vr, 0)
        for _, wave, peak, decay, sustain, release, vd, vr in songs.INSTRUMENTS))
    song_names = list(songs.SONGS)
    song_offs = [pack.add(music.compile_song(songs.SONGS[n], inst_names)) for n in song_names]
    note_off = pack.add(struct.pack("<128I", *[music.note_inc(n) for n in range(128)]))

    image_entries = []
    image_names = []
    for name, frames in entart.build():
        for i, f in enumerate(frames):
            h, w = f.shape
            if w % 2:
                raise SystemExit(f"build.py: entity image '{name}' is {w} pixels wide; widths must be even")
            image_entries.append((w, h, pack.add(pix.le16(f.reshape(-1)))))
            image_names.append(f"{name}_{i}")

    pack.align()
    region_off = pack.add(b"".join(struct.pack("<5I", *e) for e in region_entries))
    room_off = pack.add(b"".join(struct.pack("<12i", *e) for e in room_entries))
    sprite_off = pack.add(b"".join(struct.pack("<2I", *e) for e in sprite_entries))
    font_off = pack.add(b"".join(struct.pack("<3I", *e) for e in font_entries))
    image_off = pack.add(b"".join(struct.pack("<3I", *e) for e in image_entries))
    song_off = pack.add(struct.pack("<%dI" % len(song_offs), *song_offs))
    script_off = pack.add(struct.pack("<%dI" % len(script_offs), *script_offs))
    fields = {
        "magic": MAGIC, "version": VERSION, "size": len(pack.data),
        "region_count": len(region_ids), "region_off": region_off,
        "room_count": len(all_rooms), "room_off": room_off,
        "sprite_count": len(sprite_entries), "sprite_off": sprite_off,
        "font_count": len(font_entries), "font_off": font_off,
        "script_count": len(script_offs), "script_off": script_off,
        "hud_tile_count": len(hud), "hud_tiles_off": hud_off, "card_off": card_off,
        "image_count": len(image_entries), "image_off": image_off,
        "wave_count": len(music.WAVES), "wave_off": wave_off, "inst_count": len(songs.INSTRUMENTS),
        "inst_off": inst_off, "song_count": len(song_offs), "song_off": song_off, "note_off": note_off,
    }
    struct.pack_into("<%dI" % len(HEADER_FIELDS), pack.data, 0, *[fields[f] for f in HEADER_FIELDS])

    with open(os.path.join(root, "retake.pak"), "wb") as f:
        f.write(pack.data)

    lines = [
        "/* Generated by tools/build.py -- do not edit by hand. */",
        "#ifndef RETAKE_PAK_H",
        "#define RETAKE_PAK_H",
        "",
        f"#define PAK_MAGIC 0x{MAGIC:08X}",
        f"#define PAK_VERSION {VERSION}",
        f"#define PAK_SIZE {len(pack.data)}",
        f"#define PAK_ROOM_COUNT {len(all_rooms)}",
        "",
        "/* Room indices into the pack's room table. */",
    ]
    for i, room in enumerate(all_rooms):
        lines.append(f"#define ROOM_{room.name.upper()} {i}")
    lines.append("")
    lines.append("/* Sprite sheet indices into the pack's sprite table. */")
    for i, (name, frames) in enumerate(sheets):
        lines.append(f"#define SHEET_{name.upper()} {i}")
    lines.append("")
    lines.append("/* Header word indices (pak.c reads the pack through these). */")
    for i, f in enumerate(HEADER_FIELDS):
        lines.append(f"#define PAKH_{f.upper()} {i}")
    lines.append("")
    lines.append("/* Fonts. */")
    for i, (name, _) in enumerate(FONTS):
        lines.append(f"#define FONT_{name.upper()} {i}")
    lines.append("")
    lines.append("/* Intertitle scripts. */")
    for i, name in enumerate(script_names):
        lines.append(f"#define SCRIPT_{name.upper()} {i}")
    lines.append("")
    lines.append("/* HUD tiles (first tile of each 16x16 icon: TL, TR, BL, BR follow). */")
    for name, idx in hud_index.items():
        lines.append(f"#define HT_{name.upper()} {idx}")
    lines.append(f'#define HUD_TEXT_CHARS "{ui.HUD_TEXT_CHARS}"')
    lines.append("")
    lines.append("/* Entity images (frames of one entity are consecutive). */")
    for i, name in enumerate(image_names):
        lines.append(f"#define IMG_{name.upper()} {i}")
    lines.append(f"#define PAK_FLAG_COUNT {next_flag[0]}")
    lines.append("")
    lines.append("/* Songs (audio_music). */")
    for i, n in enumerate(song_names):
        lines.append(f"#define MUSIC_{n.upper()} {i}")
    lines.append("#define MUSIC_NONE 63")
    lines.append("")
    lines.append("#endif")
    with open(os.path.join(root, "pak.h"), "w") as f:
        f.write("\n".join(lines) + "\n")
    total_blocks = sum(len(block_tables[r]) for r in region_ids)
    print(f"retake.pak: {len(pack.data)} bytes, {len(all_rooms)} rooms, {total_blocks} unique blocks")


if __name__ == "__main__":
    main()
