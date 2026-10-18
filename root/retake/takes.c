#include "takes.h"

/* A jump-cut/rewind displacement is not a physical platform sweep. The
 * normal controller moves less than eight pixels per tick on either axis. */
#define CONTINUOUS_LIMIT 2048

/* Copy semantic fields explicitly; recorded layout is not a disk format. */
static void copy_frame(struct TakeFrame* to, struct TakeFrame* from) {
  to->x = from->x;
  to->y = from->y;
  to->facing = from->facing;
  to->pose = from->pose;
  to->attack_dir = from->attack_dir;
}

/* Clamp progression parameters once when entering a room or respawning. */
void takes_init(struct Takes* t, int slots, int frames) {
  if (slots < 1) slots = 1;
  if (slots > TAKE_MAX_SLOTS) slots = TAKE_MAX_SLOTS;
  if (frames < 1) frames = 1;
  if (frames > TAKE_MAX_FRAMES) frames = TAKE_MAX_FRAMES;
  t->slot_limit = slots;
  t->frame_limit = frames;
  t->recording = 0;
  t->recorded = 0;
  t->replace_next = 0;
  for (int i = 0; i < TAKE_MAX_SLOTS; i++) {
    t->slots[i].length = 0;
    t->slots[i].cursor = 0;
    t->slots[i].active = 0;
    t->slots[i].wrapped = 0;
    t->slots[i].dx = 0;
    t->slots[i].dy = 0;
    t->slots[i].fresh = 0;
  }
}

/* A newborn projection starts at frame zero and detaches any rider on the
 * slot it replaced. A one-frame still subsequently remains stable. Other
 * loop boundaries and recorded teleports disable support for one tick. */
void takes_advance(struct Takes* t) {
  for (int i = 0; i < t->slot_limit; i++) {
    struct Take* s = &t->slots[i];
    if (!s->active) continue;
    s->dx = 0;
    s->dy = 0;
    s->wrapped = 0;
    if (s->fresh) {
      s->fresh = 0;
      s->wrapped = 1;
      continue;
    }
    if (s->length == 1) continue;
    int old = s->cursor;
    s->cursor++;
    if (s->cursor >= s->length) {
      s->cursor = 0;
      s->wrapped = 1;
      continue;
    }
    s->dx = s->frames[s->cursor].x - s->frames[old].x;
    s->dy = s->frames[s->cursor].y - s->frames[old].y;
    if (s->dx > CONTINUOUS_LIMIT || s->dx < -CONTINUOUS_LIMIT ||
        s->dy > CONTINUOUS_LIMIT || s->dy < -CONTINUOUS_LIMIT) {
      s->wrapped = 1;
      s->dx = 0;
      s->dy = 0;
    }
  }
}

/* Slot ids are stable even when a projection temporarily stops supporting
 * riders; active marks whether the current frame is a continuous surface. */
int takes_platforms(struct Takes* t, struct Platform* out) {
  for (int i = 0; i < t->slot_limit; i++) {
    struct Take* s = &t->slots[i];
    struct Platform* p = &out[i];
    p->id = i;
    p->active = s->active && !s->wrapped;
    p->x = 0;
    p->y = 0;
    if (s->active) {
      p->x = s->frames[s->cursor].x;
      p->y = s->frames[s->cursor].y;
    }
    p->w = PLAYER_W * Q8;
    p->h = PLAYER_H * Q8;
    p->dx = s->dx;
    p->dy = s->dy;
  }
  return t->slot_limit;
}

/* Fill empty slots first, then replace the oldest cyclically. A completed
 * recording never aliases scratch, so the next recording cannot mutate it. */
static void finish_recording(struct Takes* t) {
  int slot = t->replace_next;
  for (int i = 0; i < t->slot_limit; i++) {
    if (!t->slots[i].active) {
      slot = i;
      break;
    }
  }
  struct Take* s = &t->slots[slot];
  for (int i = 0; i < t->recorded; i++) copy_frame(&s->frames[i], &t->scratch[i]);
  s->length = t->recorded;
  s->cursor = 0;
  s->active = 1;
  s->wrapped = 1;
  s->fresh = 1;
  s->dx = 0;
  s->dy = 0;
  t->replace_next = slot + 1;
  if (t->replace_next >= t->slot_limit) t->replace_next = 0;
  t->recording = 0;
  t->recorded = 0;
}

/* Record the result of movement and the actual hit window, not held input
 * or the whole attack animation. A press while recording includes the final
 * resolved frame, then publishes a completely independent playback copy. */
int takes_capture(struct Takes* t, struct Player* p, int record_pressed) {
  int started = 0;
  if (record_pressed && !t->recording) {
    t->recording = 1;
    t->recorded = 0;
    started = 1;
  }
  if (!t->recording) return 0;
  struct TakeFrame* f = &t->scratch[t->recorded];
  struct Hitbox box;
  f->x = p->x;
  f->y = p->y;
  f->facing = p->facing;
  f->pose = p->pose;
  f->attack_dir = player_attack_box(p, &box) ? p->attack_dir : 0;
  t->recorded++;
  if ((!started && record_pressed) || t->recorded >= t->frame_limit) {
    finish_recording(t);
    return 2;
  }
  return started;
}

/* Discard scratch only; projected takes keep their immutable paths. */
void takes_cancel(struct Takes* t) {
  t->recording = 0;
  t->recorded = 0;
}

/* A looping/flickering frame has no combat presence during its discontinuity. */
int take_attack_box(struct Take* t, struct Hitbox* box) {
  if (!t->active || t->wrapped) return 0;
  struct TakeFrame* f = &t->frames[t->cursor];
  if (!f->attack_dir) return 0;
  clapper_box(f->x, f->y, f->facing, f->attack_dir, box);
  return 1;
}

/* Validate the current room, not the scenery that existed when recorded. */
int takes_jumpcut(struct Takes* t, struct Player* p, struct Room* room, int slot) {
  if (slot < 0 || slot >= t->slot_limit) return 0;
  struct Take* s = &t->slots[slot];
  if (!s->active || s->wrapped) return 0;
  struct TakeFrame* f = &s->frames[s->cursor];
  return player_place(p, room, f->x, f->y);
}
