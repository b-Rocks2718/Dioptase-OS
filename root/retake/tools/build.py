"""Build RETAKE's asset pack (retake.pak) and its C index header (pak.h).

Run from root/retake: `python3 tools/build.py`. The pack is a flat
little-endian file that the game maps read-only (mmap of /retake/retake.pak),
so every table below is 4-byte aligned and addressed by byte offsets from the
start of the file.

Layout:
    header      magic 'RTK1', version, region_count, room_count,
                region_table_off, room_table_off, sprite_table_off,
                sprite_count, total_size
    region[i]   block_count, blocks_off, backdrop_w, backdrop_h, backdrop_off
    room[i]     region, wx, wy, w, h, blocks_off, col_off, entity_count,
                entities_off, spawn_x, spawn_y, name_off
    sprite[i]   frame_count, frames_off   (frames are 32x32 packed pixels)
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
from font import GLYPHS  # noqa: E402

MAGIC = 0x314B5452  # 'RTK1' little-endian
VERSION = 1
T = 8

ENTITY_TYPES = {}  # filled as the engine defines entity kinds


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
    header_size = 9 * 4
    pack.data += bytes(header_size)

    region_entries = []
    for r in region_ids:
        blocks = np.concatenate([b.reshape(-1) for b in block_tables[r]]) if block_tables[r] else np.zeros(0)
        boff = pack.add(pix.le16(blocks))
        back = regions[r][2]
        doff = pack.add(pix.le16(back.reshape(-1)))
        region_entries.append((len(block_tables[r]), boff, back.shape[1], back.shape[0], doff))

    room_entries = []
    for room, grid in zip(all_rooms, room_grids):
        goff = pack.add(pix.le16(grid))
        col = bytes(c for row in room.col() for c in row)
        coff = pack.add(col)
        ents = b""
        for etype, tx, ty, props in room.entities:
            code = ENTITY_TYPES.get(etype, 0)
            vals = [code, tx, ty] + [int(props.get(k, 0)) for k in ("a", "b", "c", "d", "e")]
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

    pack.align()
    region_off = pack.add(b"".join(struct.pack("<5I", *e) for e in region_entries))
    room_off = pack.add(b"".join(struct.pack("<12i", *e) for e in room_entries))
    sprite_off = pack.add(b"".join(struct.pack("<2I", *e) for e in sprite_entries))
    struct.pack_into("<9I", pack.data, 0, MAGIC, VERSION, len(region_ids), len(all_rooms),
                     region_off, room_off, sprite_off, len(sprite_entries), len(pack.data))

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
    lines.append("#endif")
    with open(os.path.join(root, "pak.h"), "w") as f:
        f.write("\n".join(lines) + "\n")
    total_blocks = sum(len(block_tables[r]) for r in region_ids)
    print(f"retake.pak: {len(pack.data)} bytes, {len(all_rooms)} rooms, {total_blocks} unique blocks")


if __name__ == "__main__":
    main()
