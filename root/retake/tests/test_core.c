/*
 * Identical native/Dioptase tests exercise the actual movement and take code.
 * Targeted cases cover variable jumps, grace/buffering, one-way crossings,
 * corner correction, wall locks, moving projection contacts, immutable
 * recording/slot limits, attacks and blocked jump cuts. A seeded long trace
 * checks solid nonpenetration and produces a host/guest parity fingerprint.
 * The tests run in user mode and use no device or scheduling assumptions.
 */
#include "../takes.h"
#ifdef RETAKE_GUEST
#include "../../crt/print.h"
#else
#include "host.h"
#endif

static unsigned char tiles[1200];
static int rows[30];
static struct Room room;
static struct Platform platforms[3];
static struct Takes bank;
static int failures;

/* Keep guest and host output identical despite their different puts APIs. */
static void emit(char* message) {
#ifdef RETAKE_GUEST
  puts(message);
#else
  fputs(message, stdout);
#endif
}

/* State the violated behavior rather than printing opaque assertion lines. */
static void expect(int condition, char* message) {
  if (!condition) {
    failures++;
    emit("***FAIL retake: ");
    emit(message);
    emit("\n");
  }
}

/* Construct a bounded room with a floor at y=160 and empty air above. */
static void make_room(void) {
  room.w = 40;
  room.h = 30;
  room.col = tiles;
  room.row = rows;
  room.platforms = platforms;
  room.platform_count = 0;
  for (int y = 0; y < 30; y++) {
    rows[y] = y * 40;
    for (int x = 0; x < 40; x++) {
      tiles[rows[y] + x] = (x == 0 || x == 39 || y == 0 || y == 20 || y == 29) ? COL_SOLID : COL_AIR;
    }
  }
}

/* Tick a stationary input for a bounded number of simulation steps. */
static void steps(struct Player* p, int count, int held) {
  for (int i = 0; i < count; i++) player_step(p, &room, held, 0);
}

/* Ground contacts remain stable and horizontal sweeps respect both walls. */
static void test_ground_and_walls(void) {
  struct Player p;
  make_room();
  player_init(&p, 32, 100, 0);
  steps(&p, 90, 0);
  expect(p.y == 148 * Q8 && p.on_ground && p.vy == 0, "falling settles on the floor without jitter");
  steps(&p, 180, IN_RIGHT);
  expect(p.x == 304 * Q8 && p.wall_dir == 1, "right wall stops the entire body at x=304");
  steps(&p, 180, IN_LEFT);
  expect(p.x == 8 * Q8 && p.wall_dir == -1, "left wall stops the entire body at x=8");
  player_init(&p, 32, 148, 0);
  steps(&p, 12, IN_RIGHT);
  int release_x = p.x;
  steps(&p, 12, 0);
  expect(p.vx == 0 && p.x - release_x < 8 * Q8, "releasing movement stops within one tile");
}

/* Measure both jump trajectories rather than asserting a copied velocity. */
static void test_jump_height(void) {
  struct Player full;
  struct Player tap;
  make_room();
  player_init(&full, 40, 148, 0);
  player_init(&tap, 80, 148, 0);
  player_step(&full, &room, IN_JUMP, IN_JUMP);
  player_step(&tap, &room, IN_JUMP, IN_JUMP);
  int full_top = full.y;
  int tap_top = tap.y;
  for (int i = 0; i < 90; i++) {
    player_step(&full, &room, IN_JUMP, 0);
    player_step(&tap, &room, 0, 0);
    if (full.y < full_top) full_top = full.y;
    if (tap.y < tap_top) tap_top = tap.y;
  }
  expect(full_top < tap_top - 20 * Q8, "holding jump produces substantially more height than a tap");
  expect(full_top > 88 * Q8 && full_top < 116 * Q8, "full jump height stays within the authored traversal envelope");
  expect(full.on_ground && tap.on_ground, "both jump variants return to stable ground");
}

/* Grace applies briefly after walking off a ledge; buffered input survives
 * until a nearby landing but never supplies unlimited mid-air jumps. */
static void test_jump_grace(void) {
  struct Player p;
  make_room();
  for (int x = 10; x < 39; x++) tiles[rows[20] + x] = COL_AIR;
  player_init(&p, 70, 148, 0);
  player_step(&p, &room, 0, 0);
  for (int i = 0; i < 20 && p.on_ground; i++) player_step(&p, &room, IN_RIGHT, 0);
  steps(&p, 2, IN_RIGHT);
  player_step(&p, &room, IN_RIGHT | IN_JUMP, IN_JUMP);
  expect((p.events & EV_JUMP) && p.vy < 0, "a jump shortly after leaving a ledge uses coyote grace");

  player_init(&p, 70, 148, 0);
  player_step(&p, &room, 0, 0);
  for (int i = 0; i < 20 && p.on_ground; i++) player_step(&p, &room, IN_RIGHT, 0);
  steps(&p, 7, IN_RIGHT);
  player_step(&p, &room, IN_RIGHT | IN_JUMP, IN_JUMP);
  expect(!(p.events & EV_JUMP), "expired coyote grace cannot create an airborne ground jump");

  make_room();
  player_init(&p, 32, 145, 0);
  p.vy = Q8;
  player_step(&p, &room, IN_JUMP, IN_JUMP);
  int jumped = (p.events & EV_JUMP) != 0;
  for (int i = 0; i < 4; i++) {
    player_step(&p, &room, IN_JUMP, 0);
    if (p.events & EV_JUMP) jumped = 1;
  }
  expect(jumped && p.vy < 0, "a buffered press launches on landing without needing a second press");
}

/* One-way tiles permit ascent and below-top passage, then catch a real
 * downward crossing; down+jump deliberately relinquishes their support. */
static void test_oneway(void) {
  struct Player p;
  make_room();
  for (int x = 8; x <= 16; x++) tiles[rows[16] + x] = COL_ONEWAY;
  player_init(&p, 80, 140, 0);
  p.vy = -1040;
  steps(&p, 12, IN_JUMP);
  expect(p.y + 12 * Q8 < 128 * Q8, "ascending body passes fully through a one-way platform");
  steps(&p, 75, 0);
  expect(p.y == 116 * Q8 && p.on_ground, "one-way platform catches feet crossing down from above");
  player_step(&p, &room, IN_DOWN | IN_JUMP, IN_JUMP);
  steps(&p, 20, 0);
  expect(p.y == 148 * Q8 && p.on_ground, "down+jump drops through and lands on the lower solid floor");
  player_init(&p, 80, 130, 0);
  p.vy = Q8;
  steps(&p, 20, 0);
  expect(p.y == 148 * Q8, "falling below the platform top does not snap the body upwards");
}

/* A two-pixel lip may be corrected; a centered full-width ceiling may not. */
static void test_corners(void) {
  struct Player p;
  make_room();
  tiles[rows[10] + 10] = COL_SOLID;
  player_init(&p, 86, 92, 0);
  p.vy = -1040;
  steps(&p, 2, IN_JUMP);
  expect(p.x == 88 * Q8 && p.y < 88 * Q8 && p.vy < 0, "right lip correction clears a two-pixel overhang");
  player_init(&p, 74, 92, 0);
  p.vy = -1040;
  steps(&p, 2, IN_JUMP);
  expect(p.x == 72 * Q8 && p.y < 88 * Q8, "left lip correction mirrors the right correction");
  player_init(&p, 80, 92, 0);
  p.vy = -1040;
  steps(&p, 2, IN_JUMP);
  expect(p.x == 80 * Q8 && p.y == 88 * Q8 && p.vy == 0, "a full ceiling stops ascent instead of correcting through it");
}

/* Holding toward a wall slows descent, and cannot cancel the next kick. */
static void test_wall_and_abilities(void) {
  struct Player p;
  make_room();
  for (int y = 1; y < 20; y++) tiles[rows[y] + 20] = COL_SOLID;
  player_init(&p, 152, 60, 0);
  p.vy = 800;
  player_step(&p, &room, IN_RIGHT, 0);
  expect(p.wall_dir == 1 && p.vy <= Q8, "pressing into the wall caps falling speed");
  player_step(&p, &room, IN_RIGHT | IN_JUMP, IN_JUMP);
  expect((p.events & EV_WALLJUMP) && p.vx < 0 && p.vy < 0, "wall jump separates from the wall and rises");
  steps(&p, 3, IN_RIGHT | IN_JUMP);
  expect(p.vx < 0, "wall-jump input lock resists held input into the old wall");

  make_room();
  player_init(&p, 40, 80, AB_WIRE);
  player_step(&p, &room, IN_JUMP, IN_JUMP);
  expect((p.events & EV_WIRE) && !p.air_jump_left, "wire gives one airborne jump");
  steps(&p, 4, 0);
  player_step(&p, &room, IN_JUMP, IN_JUMP);
  expect(!(p.events & EV_WIRE), "wire cannot repeat without regaining support");
  steps(&p, 90, 0);
  expect(p.air_jump_left == 1 && p.on_ground, "landing refills the wire jump");
  player_init(&p, 40, 60, AB_GLIDE);
  p.vy = 700;
  player_step(&p, &room, IN_JUMP, 0);
  expect(p.gliding && p.vy <= Q8 && (p.events & EV_GLIDE), "umbrella glide slows descent and emits its start event");
}

/* Deliberate reversal must brake before a tile-wide ledge is lost, while
 * a last-frame buffered jump must catch a newly reached wall immediately. */
static void test_direction_and_wall_buffer(void) {
  struct Player p;
  make_room();
  player_init(&p, 80, 148, 0);
  p.vx = 512;
  steps(&p, 4, IN_LEFT);
  expect(p.vx < 0 && p.x < 83 * Q8,
    "ground reversal turns within four ticks and three pixels of drift");
  player_init(&p, 80, 60, 0);
  p.vx = -512;
  steps(&p, 5, IN_RIGHT);
  expect(p.vx > 0 && p.x > 76 * Q8,
    "air reversal turns within five ticks and four pixels of drift");

  for (int y = 1; y < 20; y++) tiles[rows[y] + 20] = COL_SOLID;
  player_init(&p, 147, 60, 0);
  p.vx = 512;
  p.jump_buffer = 1;
  player_step(&p, &room, IN_RIGHT | IN_JUMP, 0);
  expect((p.events & EV_WALLJUMP) && p.vx < 0 && p.vy < 0 && !p.jump_buffer,
    "the last buffered frame launches when the wall comes within jump reach");
  steps(&p, 3, IN_RIGHT | IN_JUMP);
  expect(p.vx < 0, "stronger reversal braking does not cancel a protected wall kick");

  player_init(&p, 146, 60, 0);
  p.vx = 512;
  p.jump_buffer = 1;
  player_step(&p, &room, IN_RIGHT | IN_JUMP, 0);
  player_step(&p, &room, IN_RIGHT | IN_JUMP, 0);
  expect(!(p.events & EV_WALLJUMP) && !p.jump_buffer,
    "a buffer that expires before reaching the wall cannot create a later jump");
}

/* A small reach margin forgives early/late presses but never supplies a
 * phantom slide, unlimited kicks, or a jump from across a wider gap. */
static void test_wall_forgiveness(void) {
  struct Player p;
  make_room();
  for (int y = 1; y < 20; y++) tiles[rows[y] + 20] = COL_SOLID;
  player_init(&p, 149, 100, 0);
  p.vy = 500;
  player_step(&p, &room, 0, 0);
  expect(!p.wall_dir && p.vy > 500 && p.pose == POSE_FALL,
    "jump reach alone does not slow a fall or show a wall-slide pose");
  player_step(&p, &room, IN_JUMP, IN_JUMP);
  expect((p.events & EV_WALLJUMP) && p.vx < 0,
    "jumping three pixels from a right wall succeeds without directional input");
  int returned = 0;
  for (int i = 0; i < 32; i++) {
    player_step(&p, &room, IN_RIGHT | IN_JUMP, 0);
    if (p.wall_dir == 1) { returned = 1; break; }
  }
  expect(returned && p.y < 90 * Q8,
    "holding toward the wall regains it above launch height within a half-second");
  player_step(&p, &room, IN_RIGHT, 0);
  player_step(&p, &room, IN_RIGHT | IN_JUMP, IN_JUMP);
  expect((p.events & EV_WALLJUMP) && p.vy < 0,
    "a fresh press after returning to the wall chains another climbing kick");

  player_init(&p, 171, 60, 0);
  player_step(&p, &room, IN_JUMP, IN_JUMP);
  expect((p.events & EV_WALLJUMP) && p.vx > 0,
    "the same three-pixel allowance works on the left wall");
  player_step(&p, &room, IN_JUMP, IN_JUMP);
  expect(!(p.events & EV_WALLJUMP), "repeated presses cannot reuse a departing wall during the kick lock");
  player_init(&p, 148, 60, 0);
  player_step(&p, &room, IN_JUMP, IN_JUMP);
  expect(!(p.events & EV_WALLJUMP), "a four-pixel gap is outside the bounded wall-jump margin");
}

/* Initialize a synthetic take-sized platform with a stable non-index id. */
static void set_platform(int x, int y, int width) {
  room.platform_count = 1;
  platforms[0].x = x * Q8;
  platforms[0].y = y * Q8;
  platforms[0].w = width * Q8;
  platforms[0].h = 12 * Q8;
  platforms[0].dx = 0;
  platforms[0].dy = 0;
  platforms[0].active = 1;
  platforms[0].id = 7;
}

/* Carries follow continuous deltas, while loop resets and obstructed carries
 * release the rider without transporting or embedding the live body. */
static void test_platforms(void) {
  struct Player p;
  make_room();
  set_platform(32, 120, 16);
  player_init(&p, 36, 108, 0);
  player_step(&p, &room, 0, 0);
  expect(p.riding == 7 && p.on_ground, "a stationary take head supports the player");
  platforms[0].x += 2 * Q8;
  platforms[0].dx = 2 * Q8;
  player_step(&p, &room, 0, 0);
  expect(p.x == 38 * Q8 && p.riding == 7, "horizontal carry preserves the rider offset");
  platforms[0].dx = 0;
  platforms[0].dy = -2 * Q8;
  platforms[0].y -= 2 * Q8;
  player_step(&p, &room, 0, 0);
  expect(p.y == 106 * Q8 && p.riding == 7, "vertical carry rises with the projected head");
  platforms[0].active = 0;
  platforms[0].x = 200 * Q8;
  platforms[0].dx = 0;
  platforms[0].dy = 0;
  player_step(&p, &room, 0, 0);
  expect(p.x == 38 * Q8 && p.riding == -1 && !p.on_ground, "a loop reset detaches without teleporting the rider");

  make_room();
  set_platform(32, 24, 16);
  player_init(&p, 36, 12, 0);
  player_step(&p, &room, 0, 0);
  platforms[0].dy = -8 * Q8;
  platforms[0].y = 16 * Q8;
  player_step(&p, &room, 0, 0);
  expect(p.y >= 8 * Q8 && p.riding == -1, "a projected platform cannot carry its rider into a ceiling");

  make_room();
  for (int y = 1; y < 20; y++) tiles[rows[y] + 20] = COL_SOLID;
  set_platform(148, 120, 16);
  player_init(&p, 152, 108, 0);
  player_step(&p, &room, 0, 0);
  platforms[0].x += 4 * Q8;
  platforms[0].dx = 4 * Q8;
  player_step(&p, &room, 0, 0);
  expect(p.x == 152 * Q8 && p.riding == -1, "an obstructed sideways carry detaches even while heads still overlap");
}

/* Live attacks have a bounded active window, and damage cannot repeatedly
 * interrupt the same invulnerability window. */
static void test_combat_and_placement(void) {
  struct Player p;
  struct Hitbox box;
  make_room();
  player_init(&p, 32, 148, 0);
  player_step(&p, &room, 0, IN_ATTACK);
  /* The side box starts 2 px inside the leading edge (x 32 + 8 - 2), so an
   * enemy already touching Ruby is in reach; the snap stays live 6 ticks.
   * Both widened after playtesting found the clap too short to trade hits. */
  expect(player_attack_box(&p, &box) && box.x == 38 * Q8, "clapper attack is active immediately on the facing side");
  steps(&p, 5, 0);
  expect(player_attack_box(&p, &box), "clapper snap is still live five ticks after the press");
  steps(&p, 1, 0);
  expect(p.attack_timer > 0 && !player_attack_box(&p, &box), "clapper recovery animation does not keep damaging enemies");
  expect(player_hurt(&p, -1) && p.vx > 0 && (p.events & EV_HURT), "damage recoils away from its source");
  expect(!player_hurt(&p, 1) && p.vx > 0, "invulnerability rejects a second hit and preserves recoil");
  tiles[rows[19] + 4] = COL_SPIKE;
  player_init(&p, 32, 148, 0);
  player_step(&p, &room, 0, 0);
  expect((p.events & EV_HAZARD) && !player_can_place(&room, 32 * Q8, 148 * Q8), "spike overlap reports a hazard and rejects teleport placement");
  expect(!player_place(&p, &room, 0, 148 * Q8) && p.x == 32 * Q8, "blocked placement leaves the player unchanged");
  expect(player_place(&p, &room, 56 * Q8, 148 * Q8) && p.x == 56 * Q8, "clear placement succeeds without an implicit room change");
  struct Hitbox a;
  a.x = 0; a.y = 0; a.w = 8; a.h = 8;
  box.x = 8; box.y = 0; box.w = 8; box.h = 8;
  expect(!hitbox_overlap(&a, &box), "touching attack-box edges are not a hit");
}

/* Recorded paths remain independent of scratch/live state, honor slot and
 * frame limits, and never reuse movement through closed scenery for a cut. */
static void test_recordings(void) {
  struct Player p;
  struct Hitbox live;
  struct Hitbox film;
  make_room();
  player_init(&p, 32, 148, AB_JUMPCUT);
  takes_init(&bank, 1, 3);
  expect(takes_capture(&bank, &p, 1) == 1, "record press starts a take with the current resolved pose");
  p.x = 34 * Q8;
  player_step(&p, &room, 0, IN_ATTACK);
  takes_capture(&bank, &p, 0);
  p.x = 36 * Q8;
  expect(takes_capture(&bank, &p, 0) == 2 && !bank.recording, "frame limit automatically commits the take");
  expect(bank.slots[0].length == 3 && bank.slots[0].frames[0].x == 32 * Q8, "playback stores resolved positions in order");
  takes_advance(&bank);
  takes_advance(&bank);
  expect(bank.slots[0].cursor == 1 && bank.slots[0].dx == 2 * Q8, "playback advances by the recorded delta");
  p.x = 34 * Q8;
  expect(player_attack_box(&p, &live) && take_attack_box(&bank.slots[0], &film) &&
    live.x == film.x && live.y == film.y && live.w == film.w && live.h == film.h,
    "recorded active attack uses exactly the live clapper geometry");
  p.x = 90 * Q8;
  takes_capture(&bank, &p, 1);
  expect(bank.slots[0].frames[0].x == 32 * Q8, "a new scratch recording cannot mutate existing playback");
  takes_cancel(&bank);
  takes_advance(&bank);
  takes_advance(&bank);
  expect(bank.slots[0].wrapped && bank.slots[0].cursor == 0 && bank.slots[0].dx == 0,
    "loop boundary reports a discontinuity with no transport delta");
  takes_platforms(&bank, platforms);
  expect(!platforms[0].active, "loop boundary cannot support a rider");
  takes_advance(&bank);
  tiles[rows[18] + 4] = COL_SOLID;
  expect(!takes_jumpcut(&bank, &p, &room, 0) && p.x == 90 * Q8,
    "jump cut refuses scenery that closed over the recorded destination");
  tiles[rows[18] + 4] = COL_AIR;
  expect(takes_jumpcut(&bank, &p, &room, 0) && p.x == 34 * Q8,
    "jump cut reaches a clear displayed frame");

  takes_init(&bank, 9, 900);
  expect(bank.slot_limit == 3 && bank.frame_limit == 480, "upgrade limits remain within allocated storage");
  takes_init(&bank, 3, 1);
  for (int i = 0; i < 4; i++) {
    p.x = (32 + i * 8) * Q8;
    takes_capture(&bank, &p, 1);
  }
  expect(bank.slots[0].frames[0].x == 56 * Q8 && bank.slots[1].frames[0].x == 40 * Q8 &&
    bank.slots[2].frames[0].x == 48 * Q8, "a fourth take replaces only the oldest of three slots");
  takes_advance(&bank);
  takes_advance(&bank);
  takes_platforms(&bank, platforms);
  expect(platforms[0].active && !bank.slots[0].wrapped, "a single-frame still remains a stable stand after birth");
}

/* Check solid containment independently from the simulation's collision
 * helpers, so a shared mistake in those helpers cannot hide penetration. */
static int clear_of_solids(struct Player* p) {
  if (p->x < 0 || p->y < 0 || p->x + 8 * Q8 > 320 * Q8 || p->y + 12 * Q8 > 240 * Q8) return 0;
  int left = p->x / (8 * Q8);
  int right = (p->x + 8 * Q8 - 1) / (8 * Q8);
  int top = p->y / (8 * Q8);
  int bottom = (p->y + 12 * Q8 - 1) / (8 * Q8);
  for (int y = top; y <= bottom; y++) {
    for (int x = left; x <= right; x++) {
      if (tiles[y * 40 + x] == COL_SOLID) return 0;
    }
  }
  return 1;
}

/* Fixed-seed mixed actions stress geometry, attachments, record limits and
 * event ordering. The returned fingerprint is compared across compilers. */
static unsigned test_long_trace(void) {
  struct Player p;
  make_room();
  for (int x = 8; x < 17; x++) tiles[rows[15] + x] = COL_ONEWAY;
  for (int x = 22; x < 30; x++) tiles[rows[12] + x] = COL_SOLID;
  player_init(&p, 32, 148, AB_WIRE | AB_GLIDE | AB_JUMPCUT);
  takes_init(&bank, 3, 90);
  unsigned seed = 845127;
  unsigned hash = 1;
  int previous = 0;
  int valid = 1;
  for (int tick = 0; tick < 6000; tick++) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    int held = (seed >> 8) & 127;
    int pressed = held & ~previous;
    previous = held;
    takes_advance(&bank);
    room.platform_count = takes_platforms(&bank, platforms);
    player_step(&p, &room, held, pressed);
    takes_capture(&bank, &p, pressed & IN_RECORD);
    if (!clear_of_solids(&p)) valid = 0;
    hash = (hash << 5) | (hash >> 27);
    hash ^= (unsigned)p.x;
    hash ^= (unsigned)p.y << 8;
    hash ^= (unsigned)p.vx << 16;
    hash ^= (unsigned)p.vy;
    hash ^= p.events;
    hash ^= (unsigned)bank.slots[0].cursor << 20;
  }
  expect(valid, "6000 mixed-action ticks never enter solid terrain or leave the room");
  return hash;
}

/* A spring/pogo impulse detaches from films and ignores jump-release cutting. */
static void test_forced_launch(void) {
  struct Player p;
  make_room();
  player_init(&p, 80, 148, AB_WIRE);
  p.riding = 1;
  p.coyote = 5;
  p.jump_buffer = 4;
  p.jump_hold = 8;
  p.jump_cut = 1;
  p.drop_timer = 5;
  p.air_jump_left = 0;
  player_launch(&p, 1500);
  expect(p.riding == -1 && !p.on_ground && !p.coyote && !p.jump_buffer &&
         !p.jump_hold && !p.jump_cut && !p.drop_timer && p.air_jump_left,
         "a forced launch clears stale contacts and inputs, and refills the wire");
  player_step(&p, &room, 0, 0);
  expect(p.vy < -1400 && p.y < 143 * Q8,
         "a spring launch rises at full speed with Jump released");
}

/* Emit a portable decimal fingerprint without relying on variadic printf. */
static void emit_unsigned(unsigned value) {
  char digits[12];
  int end = 11;
  digits[end] = 0;
  do {
    end--;
    digits[end] = '0' + value % 10;
    value /= 10;
  } while (value);
  emit(digits + end);
}

/* Run the same assertions and trace in native sanitizers and on Dioptase. */
int main(void) {
  failures = 0;
  test_forced_launch();
  test_ground_and_walls();
  test_jump_height();
  test_jump_grace();
  test_oneway();
  test_corners();
  test_wall_and_abilities();
  test_direction_and_wall_buffer();
  test_wall_forgiveness();
  test_platforms();
  test_combat_and_placement();
  test_recordings();
  unsigned trace = test_long_trace();
  if (failures) return 1;
  emit("***PASS retake motion and recorded takes; trace ");
  emit_unsigned(trace);
  emit("\n");
  return 0;
}
