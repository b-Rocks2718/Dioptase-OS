# RETAKE — collaboration log (Claude Opus + GPT-6 Astra)

Canonical integration copy of the shared log required by `/home/brooks/PROTOCOL.md`.
Astra keeps its own checkpoint log in its top-level branch; entries are
reconciled here when its commits are integrated.

## Concept (chosen 22:50 CDT)

**RETAKE.** A precision-platforming exploration game with combat. A stunt
double is trapped on the backlot of Silverlake Pictures, a 1937 film studio
whose experimental "Everlasting Camera" captured its cast inside their roles.
She escaped only because she was never in the credits. Her clapperboard is
both her weapon (snap = attack) and a camera: holding it records a *take*, a
ghost double that replays her exact moves on a loop.

Decisions proposed by Opus. Astra leaned RETAKE, and the human said "I like
the ghost mechanics." Astra may still adjust the details.

1. **Takes are kinematic recordings.** Each tick stores x, y, pose and an
   attack flag; playback ignores the world (a projected film phases through
   doors that have closed since). A take is:
   - a moving one-way platform; standing on it carries you by its per-tick
     delta and never alters the recording
   - a body that presses plates
   - a hitbox during its recorded attack frames
   - a decoy that enemies may target

   Takes loop with a flicker back to their first frame. One slot at start
   (upgradable to 3); about 4 s base length (upgradable).
2. **Clapperboard weapon and recorder.** Movement unlocks are film-editing
   verbs: JUMP CUT (teleport to your take), OVERCRANK (slow motion), REWIND
   (undo the last ~2 s), stunt WIRE (double jump), UMBRELLA (glide).
3. **Story and lore.** Found reels play as silent-film intertitle cards;
   memos, scripts and posters add more. NPCs are actors stuck repeating their
   lines. The prop department is the shop (paid in studio scrip). Dialogue is
   shown as intertitle cards (silent-film style).
4. **Visual arc through film history.** The silent era is grayscale with
   grain, then tinted stock, two-strip Technicolor, then full color by the
   end. Eight genre sets, each with its own mechanic, music and boss. Region 1
   is a silent slapstick city set (clock towers, scaffolding: Harold Lloyd and
   Buster Keaton stunt territory).
5. **Validation.** Every challenge template ships with a recorded solution
   input script, and the real controller replays it in host tests at build
   time.
6. **Vertical slice first:** controller + takes + clapper + one enemy + one
   take-puzzle room + one boss pattern, in grayscale silent-era art. World
   production comes after.

## Requirements from the human

- **Genre:** a 2D platformer in the spirit of Celeste and Hollow Knight (both
  were "lots of fun"), but not derivative of them. The moth/monastery
  THREADLIGHT concept was rejected as too close.
- **Pillars** (from Astra's poll): "A blend: exploration, precise movement,
  and combat."
- **Scale:** a real, complete game. DELVE (8 procedural 3D levels, 10+ minute
  runs, a few attempts to win) is the bare minimum; going far beyond it is the
  goal.
- **Discovery:** "store and lore and stuff to discover." Read as story + lore,
  and possibly a shop; we plan for both.
- **Subagents:** cheap subagents may be used to automate things, but not
  excessively; keep an eye on usage. The human expects them to make the
  larger scale feasible.

## Scale target (agreed)

8 regions; about 100–140 meaningful rooms built from challenge families plus
authored landmarks; 8 bosses; 12+ enemy behaviors (re-skinned per genre);
NPCs, shop, lore, map, save, ending; first-clear time measured in playtests
(the hope is 2–4 h). A fixed persistent campaign assembled and validated at
build time; optional reshuffled expeditions later.

## Ownership

| Area | Owner |
| --- | --- |
| Engine core: controller, collision, takes, enemy/boss AI, world data validator (solution replay), save format | Astra |
| Art generators, renderer, film effects, audio/music, UI (title, HUD, intertitles, shop, map), story/lore writing, ASCII room/challenge drafting, main-loop integration | Opus |
| Level design | joint: Opus drafts, Astra validates |
| Cheap subagents | bounded volume tasks with checkable briefs on their own branches; every output reviewed by Opus or Astra before merge |

## Branches and worktrees

- Opus integration: `/home/brooks/dioptase-collab-opus`, branch
  `peers/retake` in the superproject and in each submodule worktree
  (worktrees of the main checkout's module repos). Bases: root 4821a57,
  OS 3ada75d, assembler 9a50e26, full emulator 453568b, compiler 662da26.
- Astra: `/home/brooks/dioptase-resonance-astra` (independent clones). Its
  branch is to become `peers/retake-logic`. Its OS commit 7899286 is an
  obsolete pinball header and must not be integrated.
- The demo lives in `Dioptase-OS/root/retake/`; the binary is `/sbin/retake`.

## Architecture decisions

- Display (verified by a spike on the emulator, 60 fps locked):
  - Tile layer at tile scale 1 is an 80x60-tile wrapping plane of 8x8 tiles
    in 320x240 space. A room of at most 80x60 tiles is loaded whole; the camera
    is the tile scroll registers (`scroll = -camera`). This relies on the full
    emulator's implementation (`graphics.rs`: scroll is added before the
    640x480 wrap, then scaled), which `docs/mem_map.md` does not spell out.
  - Pixel layer (under the tiles) holds the parallax backdrop, enemies and
    particles, redrawn each frame with an asm word copier.
  - Hardware sprites (on top; scale 1 takes 320-space coordinates, again per
    `graphics.rs`) hold the hero, takes and HUD.
  - Pixel rows 236–239 share a page with device registers and are not
    user-mappable, so the bottom 4 rows stay black (a film letterbox).
- Physics: fixed 60 Hz ticks, Q8 positions, 8x12 hero body, 8 px tiles.
- bcc limits to remember:
  - no typedef/float/long/varargs
  - array sizes must be integer literals
  - software multiply costs ~75 instructions for a 15-bit second operand
    (smul loops over |r2|'s bits), so hot paths use shifts or precomputed row
    offsets

## Changes outside the demo

None yet.

## Usage status

- 19:25 CDT: Opus 5h 24% (resets 22:50 CDT), weekly 9%.
- 19:44 CDT: Astra hit its limit; out until 22:22 CDT.
- 22:33 CDT: Opus 5h 70% (window resets 22:50 CDT), weekly 13%.
- 22:40 CDT: Astra 5h 27%, weekly 20%; its 5h window resets 03:32 CDT Oct 18.

## History

- First agreed concept: RESONANCE (musical pinball). Dropped when the human
  asked for a platformer.
- Second: THREADLIGHT (moth courier in a crystal monastery). The human found
  it too derivative.
- Round-2 brainstorm (`IDEAS.md`, plus Astra's COUNTERWEIGHT / CINDERLINE /
  CUTOUT): RETAKE chosen.

## Status

- 19:43 CDT: render spike committed (graybox room, parallax, sprite hero).
- 22:50 CDT: RETAKE chosen. Folder renamed to `root/retake`, branches to
  `peers/retake`. Opus starting the art pipeline, renderer and main loop;
  waiting on Astra's controller + take header.
