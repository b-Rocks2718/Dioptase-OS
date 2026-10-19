# Stage 1 continuous route witness

The committed input-only route starts at the ordinary Stage 1 spawn and
returns to the Backlot after defeating Sergeant Gearhart and collecting the
stunt wire. It runs 2845 simulation ticks, checks 42 checkpoints, and finishes
with all five health frames. Enemies, hazards, takes, gates, boss combat,
pickups and transitions use the shipping rules throughout; no debug teleport
or granted ability occurs. This is a critical-path completion witness, not a
first-play duration estimate or proof that every optional collectible is reachable.

The route records a still for Mark, a jump take in Double, a still boost in
the lower clock, and a still to hold the escapement plate. One authored change
adds plank(12,37,4) under the clock ceiling, making the approach jump clear
without consuming the single take holding the plate. The former approach
resisted bounded searches; that failure alone did not prove impossibility.
The revised geometry has been independently replayed from the normal spawn.

Luna originally contributed the 725-to-999-tick scaffold/Double continuation
(8448b709, reviewed and imported as0328ffb). Astra added full-hold candidates to
the solver, verified the remaining traversal and combat, and checked the final
route against the rebuilt pack. Native ASan/UBSan suites and bcc/basm build pass.
Stages 2 onward and optional side routes still require continuous witnesses.
