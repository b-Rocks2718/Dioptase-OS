# Late-stage local traversal witnesses

These 13 fixtures retain the ledge/exit checkpoints authored by Opus in
`reach.py` and `reach2.py`, with inputs re-authored by Astra after the movement
polish. Each starts at explicit local coordinates with that stage's abilities;
none claims to prove unlocking those abilities, the full campaign, or human
play time. Enemy damage, hazards, world timers and transitions remain active.
All finish with five health under controller checkpoint 7657106.

Run from root/retake after `make -C tests build/sim.so`:

```sh
python3 tools/replay.py tests/routes/late/*.json
```

Coverage: Moon craters, moving hover pads and reactor shaft; Night City alley,
roofs, billboard, sniper crossing and tower; Revue wings, stage, beat-block
flyloft and catwalk, and grand hall. Searches use short/full jump candidates
and intermediate authored ledges for the difficult climbs. Every successful
search is independently replayed before writing the witness. A changed pack
hash triggers revalidation, not automatic rejection or automatic acceptance.

Not covered here: laser timing under Overcrank, take-held curtain gates,
boss victories, optional collectibles, and connected campaign progression.
Searchlight crossing is a valid route through active rules, not proof that
using a decoy is necessary. Tests of decoy priority belong in test_game.c.
