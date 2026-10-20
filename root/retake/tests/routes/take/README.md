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
- `s5_lab-generator.json`: the door to the Monster wants three slates held
  at once (a lamp each), each holding one second and too far apart for Ruby
  alone. Two takes clap the first two from the girders; Ruby claps the third
  by the door. `check_escapes.py` finds no route to the arena without takes.
  One take can also do it: a loop that claps both girder slates wraps
  straight from the second back to the first, so their holds overlap. With
  a 114-tick loop, Ruby clapping the third slate got through for 37 of 50
  tried waits (checked once by hand, not part of the suite).
- `s7_club-spotlight.json`: the club's back door opens only while the stage
  spot (no rifle) is locked on a take. Ruby leaves a still on the bandstand,
  the spot finds it and stays, and she walks out to the roofs.
  `check_escapes.py` finds no route out of the club without a take.
