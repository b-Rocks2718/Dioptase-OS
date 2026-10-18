# Platformer brainstorm, round 2 (Opus)

The human's feedback on THREADLIGHT: be more creative and less derivative of
Hollow Knight and Celeste. The moth protagonist in particular read as a Hollow
Knight bug. What should carry over is what they enjoyed in those games and
named in Astra's poll: exploration, precise movement and combat. Each idea
below gets its own identity from a core verb, not just a reskin.

The working folder is still named `threadlight` for the render spike. It gets
renamed once we pick.

## 1. RETAKE — a stunt double and her recorded takes

A stunt double is trapped on the backlot of a decaying 1930s film studio
where the cameras never stopped rolling. Press record, and your next few
seconds become a *take*: a flickering, sepia ghost double that replays your
exact moves on a loop. You can stand on your double's head, leave it holding
a switch, or have it swing at an enemy while you dodge.

- Exploration: each region is a movie set (a Western town, a pirate galleon,
  a moon-base serial, noir rooftops), each with its own palette and score.
  Lost film reels are the collectibles.
- Movement: chaining takes lets you build routes you couldn't run alone.
- Combat: stunt fights where your doubles land hits beside you. Bosses are the
  studio's "monsters" (the rubber-suit sea serpent, the stop-motion giant).
- Hardware: deterministic 60 Hz input replay is nearly free. Doubles are
  tinted hardware sprites. The unmappable 4-pixel bottom strip becomes a
  deliberate film letterbox.
- Risk: room design has to teach the take system gently. The engine side is
  small.

## 2. WIND-UP — a spark that possesses toys

You are the spark that animated an old toymaker's workshop. On your own you
are a weak wisp that can barely float. Dive into a wind-up toy to inhabit it:

- tin frog: charged jumps
- paper glider: gliding
- clockwork crab: walks on walls and ceilings
- nutcracker: sword fighting, heavy
- spinning top: drill-dash

Enemies are rogue toys, and beating one means taking it over. Movement and
combat become one verb. The world is a giant workshop: shelves, a toy train
line, a dollhouse, the music box. Areas are gated by which toy you're riding.

- Risk: each toy needs its own controls, animation and AI, so this is the most
  content of any idea here. Scope it to 4–5 toys.

## 3. PIGMENT — painting a gray world

A gray pencil figure escapes an unfinished drawing. Its brush paints surfaces
with properties:

- blue: bounce
- green: grip (wall climbing)
- red: burn (hurts enemies, clears brambles)
- yellow: speed

Each new pigment is a metroidvania key, and the world visibly fills with
color as you play. Regions are different art media: pencil, watercolor, ink
wash, stained glass. In combat, paint affects enemies too: blue knocks them
away, green glues them in place, red damages.

- Hardware: every tile entry carries an RGB332 color byte that recolors the
  tile's 0xC pixels, so painting a surface is a single byte write.
- Risk: painting has to feel instant (a thrown splat, not a menu).

## 4. ALMANAC — a bell that changes the season

A wandering bell-ringer rings the current room into a new season:

- winter freezes waterfalls into climbable walls and lakes into floors
- spring grows vines and bounce-blossoms
- summer bakes the mud dry and raises thermals
- autumn gales carry you and drop platforms of leaves

Every room is four puzzles in one, and switching seasons mid-jump is the
precision skill. Enemies change with the seasons; frozen ones become
platforms.

- Hardware: re-uploading the 256-tile set and the background changes the
  whole room in one frame.
- Risk: four art variants of every tileset.

## 5. TWOFOLD — every room has a reflected twin

Each room exists twice, awake and dreaming. Flip between them at any moment,
including mid-jump. A platform in one world is spikes in the other. Enemies
live in one world and cast shadows into the other.

- Hardware: the two versions of a room sit in the two halves of the tile
  plane, so a flip is a single scroll-register write.
- Risk: the mechanic itself is well known (Giana Sisters, Shift). The setting
  has to carry the originality.

## 6. TETHER — a grappling line in a vertical city

A window-washer swings up a mile-high tower on a grappling line: swing, reel
in and out, slingshot, yank enemies off ledges, zip between anchors. The
movement is all momentum, which feels very different from Celeste's dash.

- Risk: rope collision edge cases. It suits Astra's physics strengths.

## 7. INTERRUPT — a stray signal inside a living computer

A lost interrupt races through a computer imagined as a world: cache lakes,
pipeline conveyors, a TLB maze. `fork()` leaves an echo (RETAKE's mechanic),
`sleep` freezes time, and the last boss is Kernel Panic. It would make the
human's two years of work into the game's world.

- Risk: the human asked for cool software rather than an OS feature
  showcase, and this setting leans that way.

## Smaller hooks that could combine with any of the above

- **Stillframe:** freeze time for a beat, and frozen enemy projectiles become
  stepping stones.
- **Small Worlds:** radial gravity around tiny planetoids (Mario Galaxy in
  2D). Very fresh, but less precise, and it needs a different renderer.
- **Cadence:** platforms and enemies pulse to the soundtrack's beat. Risky,
  because emulator audio latency could drift.

## Opus ranking

1. **RETAKE.** The premise is fresh, it serves all three requested pillars,
   the engine work stays small (deterministic replay), and the film-studio
   sets give a strong visual identity unlike either reference.
2. **WIND-UP.** The most original core loop, since every enemy is also a
   power-up. The cost is content.
3. **PIGMENT.** The best visual payoff and the best fit with the hardware (tile
   color bytes), with clear ability gating.

We're waiting for Astra's ideas before choosing. The human paused work at
about 19:50 CDT so Astra can contribute when it returns (~22:22 CDT).

## Addendum (22:35 CDT): scale and story

Two more inputs from the human. First, DELVE (8 procedural 3D levels, a run of
over 10 minutes, a few attempts to win) is the bare minimum, and far beyond it
is the goal. Second, "it'd be cool if there was like store and lore and stuff
to discover." That most likely means story + lore, possibly also a shop; we
plan for both.

### How we get a game much bigger than DELVE without hand-drawing it all

- Content is produced by generators, not typed in by hand. Python tools turn
  compact room descriptions and procedural art recipes into data. A world
  builder assembles connector rooms from authored chunks (Spelunky-style
  templates) around hand-built landmark rooms (story beats, shops, bosses).
- Build-time validation: a reachability checker runs the movement model over
  every room to prove it is traversable with the abilities available at that
  point in the world. That's the only way a 100+ room world stays correct.
- Target shape: about 6–8 regions × 12–20 rooms (100+ rooms), one boss per
  region, around 10 enemy behaviors, 5–6 movement abilities, NPCs, a shop,
  lore items, a map, an ending, and roughly 1.5–3 hours for a first clear.

### What story and lore look like in my top three

- **RETAKE.** Silverlake Pictures vanished in 1937 when its experimental
  camera began capturing people on film. Lore comes as found reels that play
  as silent-film intertitle cards, plus studio memos, scripts and posters.
  Actors trapped in their roles are the NPCs. The prop department is the shop
  (paid in studio scrip). Each region is a film set with its own genre
  mechanic and boss:
  - Western: rail carts, lassos
  - pirate galleon: ropes, rising water
  - jungle serial: vines
  - creature-feature horror: darkness
  - moon serial: low gravity
  - noir city: rain, searchlights
  - musical: platforms on the beat
  - the projection booth: finale
  This is the strongest built-in variety-per-region of any idea here.
- **WIND-UP.** The toymaker vanished, and the house's rooms are the regions,
  seen at toy scale: workshop, nursery, kitchen, garden, attic, cellar, clock
  tower. Music-box cylinders replay memories as lore. The tinker's bench is
  the shop. The mystery is what happened to the toymaker's family.
- **PIGMENT.** Each region is one painting from an unfinished collection.
  Sketchbook pages and margin notes reveal why the painter stopped. The art
  supply cart is the shop.

### A structural alternative worth weighing

**Roguelite with a persistent hub** (Dead Cells / Hades style). Runs through
procedurally assembled levels; the story unfolds across runs through hub NPCs
and found journals, and a shop spends what you bring back. This scales almost
for free and matches DELVE's procedural spirit. The cost is less of the
hand-placed discovery the human asked for. A middle path: a fixed,
hand-structured world whose connector rooms are generated once at build time.
