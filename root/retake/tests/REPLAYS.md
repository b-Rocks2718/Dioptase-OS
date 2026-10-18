# Actual-game route validation

From `root/retake`:

```sh
python3 tools/build.py
make -C tests test build/sim.so
python3 tools/replay.py tests/routes/stage1-opening.json
```

The native adapter compiles the actual game.c, player.c and takes.c. Only
pack loading, rendering and file I/O are adapted. Enemy damage, collision,
take playback, gates and room transitions all remain active. The shared
library runs with fatal UBSan errors. The regression executable also uses
ASan. Snapshots include movers and every rule global, but exclude save-file
contents: route search never calls game_load. Snapshots are validated by
restoring and comparing the entire result after a repeated simulation.

The opening witness starts at the actual Stage 1 spawn, crosses Mark's gate
with a stationary take, climbs the corrected scaffold chimney, and records a
jump take below Double's ledge. It takes 999 ticks without damage. This is
**not a full Stage 1 solution or a play-duration estimate**. Double's ledge,
rooftops, clock tower, boss, and optional cellar still need continuous witnesses.
stage1-clock-local.json separately checks 346 ticks from explicit roof-entry
coordinates to the clock gate's plate. It is only a local traversal witness.

Every witness contains sequential `[tick_count, held_input_mask]` pairs.
Press edges are derived from held input, so a held jump never automatically
repeats. Checkpoints assert room and position; optional `abilities` asserts
required unlocks. Hazard/death aborts validation. A changed pack hash is
reported and the route is revalidated against the current simulation; an
art rebuild alone does not invalidate a still-working input route.

## Continuing a route

The bounded beam search in tools/solve.py is an authoring aid. It can miss
valid routes; failure is never an impossibility proof. Any successful search
is replayed independently from the starting snapshot before being returned.
Use narrow intermediate rectangles to guide complex climbs. Exact heights
are the hero's top-left: for example, standing on tile row 43 means y=332.
Room rectangles use pixels; x/y/vx/vy in Sim.state are Q8.

```python
import json
import sys
sys.path.insert(0, "tools")
from replay import Sim
from solve import waypoint

path = "tests/routes/stage1-opening.json"
route = json.load(open(path))
sim = Sim()
sim.start(**route["start"])
for segment in route["segments"]:
    sim.actions(segment["actions"])

# Choose the next checkpoint; full jump holds survive the beam's pruning.
rect = [216, 60, 280, 60]
actions = waypoint(sim, "s1_double", rect, width=128, depth=100, quantum=6)
route["segments"].append({
    "label": "Reach Double's upper ledge",
    "actions": actions,
    "expect": {"room": "s1_double", "rect": rect},
})
with open(path, "w") as output:
    json.dump(route, output, indent=2)
    output.write("\n")
```

For a take puzzle, append explicit inputs to record a useful stand or motion,
then search from that exact state. Example on a plate: `[[6,0], [1,32],
[2,0], [1,32], [1,0]]` lets Ruby stop and records a stationary take. Search
does not invent recordings. The next full replay verifies those inputs too.
Use `Sim.start(room, x=..., y=..., abilities=...)` only for labeled local
fixtures; a chain of teleported local successes is not a campaign proof.
