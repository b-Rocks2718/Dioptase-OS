# Game rules review, 2026-10-18

Baseline: Opus `99669bb`, merged into Astra `ce2454b`. Before edits,
`make -C root/retake/tests test` passes under ASan/UBSan, and
`make -j4 retake_core.summary-test user_foreground_display.summary-test
user_line_editor.summary-test TEST_RUNS=1` passes all three guest tests.
The controller trace is `3239651184`. These tests did not exercise game.c.

Investigate with the real game.c, player.c and takes.c, adapting only pack,
render and save I/O for native tests:

- Upward exits require negative velocity after the controller has stopped
  against the solid out-of-room ceiling. Check cellar and clock transitions.
- Gate closure must defer while Ruby overlaps its collision column; Opus
  explicitly agreed. Gates must never index outside the collision array.
- Forced pogo/spring/recoil motion is currently shortened by releasing jump.
- Boss rewards and return door are only created on defeat, not on re-entry.
- Lethal contact can be followed by healing or a door before death resolves.
- Signed left shifts of negative world coordinates and flyer offsets are C
  undefined behavior; use bounded signed arithmetic in the shared simulation.
- Saves are being independently reviewed; no POSIX guarantees are assumed.

Room geometry must be proved by input replay through the actual controller,
including take puzzles; a geometric flood fill is insufficient. The native
harness must retain combat and progression for any claimed campaign proof.

## First checkpoint

The native adapter reproduced all five progression failures before fixes
(`/tmp/astra-game-repro.log`). UBSan also reported negative left shifts in
world origins and flyer motion. Regression tests now pass: upward cellar
exit, blocked neighbor rejection, deferred gate closure, boss reward re-entry,
lethal spike/door ordering, and full spring impulse with Jump released.
All twelve packed rooms run 480 actual game ticks each under ASan/UBSan.

The controller exposes boundary-stopped velocities, since the game cannot
infer attempted upward movement from the already-zeroed post-collision vy.
Forced launches use player_launch and clear stale attachment/buffer state;
only voluntary jumps can be shortened by releasing Jump. Host/guest core
tests still produce trace `3239651184`; the added launch regression also
runs on the guest. The complete game builds using optimized bcc and basm.
Boss reward reconstruction now runs on room entry; encounter hazards are
cleared before reward allocation. A projected kill cannot turn the spawned
scrip into a damage source through reuse of the enemy's slot.

Outstanding: actual route witnesses, robust saving, enemy collision review,
and remaining independent-review test gaps. The cheap save/pack review
confirmed failed-save success feedback, partial overwrite, insufficient load
validation, and PAK_SIZE not identifying world layouts. No atomic rename or
write guarantee exists in the filesystem contract; saving needs a recoverable
record strategy rather than a POSIX replacement assumption.

## Stage 5/6 review, Oct 18 17:54 CDT

Baseline67b4988 merged; native suites and guest save/file/display checks pass.
Bounded Luna review reproduced crumbles losing15 timer units in10 Overcrank
ticks: step_entities' default branch and update_crumbles both decrement them.
Normal warning/gone periods are halved too. Initial collision is absent until
after the first player step, and re-forming sets state0 but delays solid
collision until the next frame, allowing Ruby to enter during that gap.
Separate timer advancement from collision maintenance, skip generic updates,
and publish re-formed collision in the same overlap-checked step.

Astra found Rewind writes x/y directly rather than using player_place, retaining
coyote grace and wall input lock from the departure. Rewind+jump from a floor
to a prior airborne point can grant an unearned ground jump. Use validated
player_place, then clear momentum and wall lock; retain health and attack state.
Focused regressions are added before these fixes, including initial/re-formed
collision and normal versus Overcrank warning lengths.

Both reproductions fail on the baseline and pass after the fixes. Crumbles
now initialize collision on room entry, tick once per world step, and restore
solidity immediately after an overlap check. Rewind uses player_place and
clears the prior wall lock. Save validation also rejects checkpoint bodies
inside a crumble that would re-form on load. Native suites, both committed
routes, the bcc/basm game build, and guest retake_save/retake_core pass.

Stage1 witness completed:2845ticks,42checkpoints,5health, wire earned and
return door used. One transfer plank(12,37,4) added below the clock ceiling;
the plate still requires a take and both gates are traversed. A mutable-grid
prototype first established the local route, then the actual world.py pack
was rebuilt and the entire continuous route replayed independently. Both
committed routes, native suites and bcc/basm build pass.
