# Movement polish, 2026-10-18

Human playtest reports awkward movement alongside animation, menu and music
issues. Opus owns presentation; Astra owns controller changes and route checks.
Baseline is 60162b4. Native ASan/UBSan suites and guest retake_core pass;
the shared trace is 3239651184. No hardware or syscall contract changes.

Measurements from the shipping controller in an empty test room:
- Full-speed reversal first moves the requested way on tick 7 on ground,
  tick 10 in air; it still drifts 5.25 / 7.97 pixels the old way meanwhile.
  Reaching full opposite speed takes 13 / 19 ticks respectively.
- A jump buffer with one tick left expires on the tick Ruby reaches a wall:
  wall_dir becomes nonzero after movement, but only grounded contacts retry
  the buffered jump. This loses valid input at the edge of the buffer window.
- test_core main resets failures after test_forced_launch, hiding its failure
  exit status. Move initialization before every test.

Plan: consume valid buffered wall contact in the arrival tick, and double
horizontal braking when input opposes velocity. Preserve run speed, jump
impulse/gravity, six-frame coyote grace, five-frame buffer and forced wall-kick
lock. Verify directional response, jump-height envelope, identical host/guest
simulation, and actual-room route witnesses. A route divergence after tuning
needs a new input witness, not a weakened checkpoint or changed room geometry.

## Reference study and result

The official Celeste player source separates its three-pixel wall-jump check
from ordinary contact, and includes jump grace and variable-height jumps:
https://github.com/NoelFB/Celeste/blob/master/Source/Player/Player.cs
William Pellen explains Hollow Knight's immediate horizontal response and
last-moment aerial control in this direct developer interview:
https://gameinformer.com/2018/10/15/the-making-of-hollow-knight
These inform responsiveness goals; RETAKE retains its own fixed-point model.

The candidate reduces reversal drift to 1.75 px / 3.44 px, with first opposite
motion on tick 4 / 5 (ground / air). Wall-jump reach is three pixels, separate
from contact/slide; it favors nearest, then facing on a tie. Six ticks of
separating lock protect the kick, and near-wall repeat presses cannot reuse
that wall during the lock. A buffered press also consumes on arrival within
reach rather than expiring. Top run speed and vertical jump arc are unchanged.
Focused tests cover both wall sides, an out-of-range gap, no phantom slide,
repeat-press rejection and same-wall climbing by holding toward the wall.
Seven assertions fail against the original controller and all pass afterward.

Native ASan/UBSan core/game/save suites, all-room rule sweep, bcc/basm game
build and guest retake_core/retake_save pass. The new shared 6000-tick trace is
1286373884 in both host and guest; tests/retake_core.ok changes to that value
because the intended controller response changed, not to hide an invariant
failure. No nonpenetration or route checkpoints were weakened. Stage 1's full
route is re-authored and independently replayed: 2798 ticks, 42 unchanged
checkpoint predicates, full health, earned wire, return to hub. The local
clock route passes 349 ticks. Subjective feel still needs peer/human playtest.
