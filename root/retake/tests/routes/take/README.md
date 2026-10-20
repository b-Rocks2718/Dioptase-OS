# Take set pieces

Witnesses for rooms built around a take mechanic. Each route replays the
intended solution through the real rules (`python3 tools/replay.py
tests/routes/take/*.json` after `make -C tests build/sim.so`).

A route shows a solution exists. Whether the mechanic is *required* is a
separate claim, checked by `check_escapes.py` with Astra's bounded search
(`tools/solve.py`): it finds no route, which is evidence, not proof.

- `s3_plank-landing.json`: the sea stack's cave is held open by a slate on
  a piling deep in the water. Ruby leaves a still on the landing, falls,
  claps the slate, cuts back to the still before she lands, and walks into
  the wreck. `check_escapes.py` drops Ruby from above to the highest point
  any clap can reach (y 183, wire unused) at three x positions and finds no
  cut-free route back to the pier, the landing, or the wreck. The reef in
  the seam columns keeps the room's edge from serving as a wall to kick up.
