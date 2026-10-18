"""Author and verify input witnesses using the shipping C simulation.

The native adapter replaces only rendering and file I/O. Every route uses
game_tick, including enemies, damage, takes, gates and room transitions.
A witness starting at the normal spawn is a continuous route; fixture starts
with explicit coordinates are only local traversal checks, never a campaign
completion claim. Run `make -C tests build/sim.so` before this tool.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
LEFT, RIGHT, UP, DOWN, JUMP, RECORD, ATTACK, CUT, USE = (1, 2, 4, 8, 16, 32, 64, 128, 256)
GE_HAZARD, GE_DEATH, GE_ROOM = 32, 64, 128
STATE_FIELDS = ("room", "x", "y", "vx", "vy", "ground", "wall", "health", "abilities",
                "events", "recording", "recorded", "take", "hold", "lock", "coyote",
                "wire", "boss_hp", "scrip", "riding")


class Sim:
    """Own one native simulation; opaque snapshots preserve all rule state."""

    def __init__(self):
        """Load the compiled adapter and resolve room names from the actual pack."""
        self.lib = ctypes.CDLL(str(ROOT / "tests/build/sim.so"))
        self.lib.sim_open.argtypes = [ctypes.c_char_p]
        self.lib.sim_state.argtypes = [ctypes.POINTER(ctypes.c_int)]
        self.lib.sim_snapshot.argtypes = [ctypes.c_void_p]
        self.lib.sim_restore.argtypes = [ctypes.c_void_p]
        if self.lib.sim_open(str(ROOT / "retake.pak").encode()) != 0:
            raise RuntimeError("native replay rejected retake.pak")
        self.snapshot_bytes = self.lib.sim_snapshot_size()
        self.buffer = (ctypes.c_int * len(STATE_FIELDS))()
        self.pack = (ROOT / "retake.pak").read_bytes()
        head = struct.unpack_from("<25I", self.pack)
        self.rooms = []
        self.names = {}
        for i in range(head[5]):
            r = struct.unpack_from("<12i", self.pack, head[6] + i * 48)
            name = self.pack[r[11]:].split(b"\0", 1)[0].decode()
            self.names[name] = i
            self.rooms.append((name, r))
        self.held = 0
        self.frames = 0

    def state(self):
        """Read a copy of compact telemetry; positions and velocities are Q8."""
        self.lib.sim_state(self.buffer)
        return dict(zip(STATE_FIELDS, self.buffer))

    def start(self, room, x=None, y=None, abilities=0):
        """Use the real new-game spawn unless an explicit local fixture is requested."""
        index = self.names[room] if isinstance(room, str) else room
        self.lib.game_new(index)
        if x is not None or y is not None or abilities:
            if x is None or y is None:
                raise ValueError("a fixture needs both x and y")
            self.lib.sim_fixture(index, x, y, abilities)
        self.held = 0
        self.frames = 0

    def tick(self, held, pressed=None):
        """Advance one real game tick; normal scripts derive press edges from held input."""
        if pressed is None:
            pressed = held & ~self.held
        self.lib.game_tick(held, pressed)
        self.held = held
        self.frames += 1
        return self.state()

    def snapshot(self):
        """Pair the C state with input-edge history for exact search restoration."""
        data = ctypes.create_string_buffer(self.snapshot_bytes)
        self.lib.sim_snapshot(data)
        return data.raw, self.held, self.frames

    def restore(self, snapshot):
        """Restore rule state and press-edge history without rerunning room initialization."""
        data, self.held, self.frames = snapshot
        self.lib.sim_restore(ctypes.c_char_p(data))

    def actions(self, actions, trace=False):
        """Replay [ticks, held] runs, rejecting death or hazards unless explicitly requested elsewhere."""
        previous_room = self.state()["room"]
        for count, held in actions:
            if count < 1 or count > 36000 or held < 0 or held > 2047:
                raise ValueError(f"invalid action {(count, held)}")
            for _ in range(count):
                s = self.tick(held)
                if s["events"] & (GE_HAZARD | GE_DEATH):
                    raise AssertionError(f"route hit a hazard/death at frame {self.frames}: {s}")
                if trace and s["room"] != previous_room:
                    print(f"frame {self.frames}: {self.rooms[s['room']][0]} at "
                          f"({s['x']/256:.1f}, {s['y']/256:.1f}), health {s['health']}")
                previous_room = s["room"]
        return self.state()


def verify(path):
    """Check checkpoints by replay, reporting when the authored pack has changed."""
    route = json.loads(path.read_text())
    sim = Sim()
    if route["pack_sha256"] != hashlib.sha256(sim.pack).hexdigest():
        print(f"{path.name}: pack changed since authoring; verifying inputs against the current build")
    sim.start(**route["start"])
    for segment in route["segments"]:
        s = sim.actions(segment["actions"], trace=True)
        expect = segment["expect"]
        if s["room"] != sim.names[expect["room"]]:
            raise AssertionError(f"{segment['label']}: expected {expect}, observed {s}")
        if "rect" in expect:
            x0, y0, x1, y1 = expect["rect"]
            if not (x0 * 256 <= s["x"] <= x1 * 256 and y0 * 256 <= s["y"] <= y1 * 256):
                raise AssertionError(f"{segment['label']}: missed checkpoint {expect['rect']}: {s}")
        if s["abilities"] & expect.get("abilities", 0) != expect.get("abilities", 0):
            raise AssertionError(f"{segment['label']}: missing required ability: {s}")
        print(f"PASS {segment['label']}: frame {sim.frames}, health {s['health']}")
    print(f"PASS {path.name}: {sim.frames} ticks ({sim.frames/60:.1f}s), {len(route['segments'])} checkpoints")


def main():
    """Validate one or more committed JSON witnesses, failing on the first divergence."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("routes", nargs="+", type=Path)
    args = parser.parse_args()
    for path in args.routes:
        verify(path)


if __name__ == "__main__":
    main()
