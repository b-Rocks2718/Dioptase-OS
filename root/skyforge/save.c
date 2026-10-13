#include "skyforge.h"
#ifndef SKYFORGE_HOST_TEST
#include "../crt/sys.h"
#include "../crt/unistd.h"
#include "../crt/fcntl.h"
#endif

/*
 * Persistent storage for SKYFORGE high scores, best Grand Prix race time,
 * and custom sculpted 3D worlds on the ext2 SD filesystem.
 * Respects the 1024-byte per-syscall read/write bound (docs/syscalls.md).
 */

#define SCORE_FILE_PATH "/skyforge/scores.sf"
#define WORLD_FILE_PATH "/skyforge/world.sf"
#define SCORE_MAGIC 0x53464849u
#define WORLD_MAGIC 0x53465744u

int g_high_scores[5] = { 2500, 1800, 1200, 800, 400 };
int g_best_lap_ticks = 0;

struct ScoreDiskRecord {
  unsigned magic;
  int scores[5];
  int best_lap_ticks;
};

struct WorldDiskHeader {
  unsigned magic;
  int biome;
  int sector;
  unsigned seed;
};

#ifndef SKYFORGE_HOST_TEST

/* Read exactly `len` bytes from `fd` in bounded 512-byte chunks. */
static bool read_all_chunks(int fd, char* dst, int len){
  int off = 0;
  while (off < len){
    int chunk = len - off;
    if (chunk > 512) chunk = 512;
    int got = read(fd, dst + off, (unsigned)chunk);
    if (got <= 0) return false;
    off += got;
  }
  return true;
}

/* Write `len` bytes to `fd` in bounded 512-byte chunks. */
static bool write_all_chunks(int fd, char* src, int len){
  int off = 0;
  while (off < len){
    int chunk = len - off;
    if (chunk > 512) chunk = 512;
    int put = write(fd, src + off, (unsigned)chunk);
    if (put <= 0) return false;
    off += put;
  }
  return true;
}

/* Load persistent high scores and best race time from /skyforge/scores.sf. */
void scores_load(void){
  int fd = open_existing(SCORE_FILE_PATH);
  if (fd < 0) return;
  struct ScoreDiskRecord rec;
  bool ok = read_all_chunks(fd, (char*)&rec, sizeof(struct ScoreDiskRecord));
  close(fd);
  if (!ok || rec.magic != SCORE_MAGIC) return;
  for (int i = 0; i < HIGH_SCORE_SLOTS; ++i){
    g_high_scores[i] = rec.scores[i];
  }
  g_best_lap_ticks = rec.best_lap_ticks;
}

/* Insert `score` and/or `race_ticks` into the leaderboard and save to disk. */
void scores_record(int score, int race_ticks){
  if (score > 0){
    for (int i = 0; i < HIGH_SCORE_SLOTS; ++i){
      if (score > g_high_scores[i]){
        for (int j = HIGH_SCORE_SLOTS - 1; j > i; --j){
          g_high_scores[j] = g_high_scores[j - 1];
        }
        g_high_scores[i] = score;
        break;
      }
    }
  }
  if (race_ticks > 0 && (g_best_lap_ticks == 0 || race_ticks < g_best_lap_ticks)){
    g_best_lap_ticks = race_ticks;
  }

  int fd = open(SCORE_FILE_PATH);
  if (fd < 0) return;
  struct ScoreDiskRecord rec;
  rec.magic = SCORE_MAGIC;
  for (int i = 0; i < HIGH_SCORE_SLOTS; ++i){
    rec.scores[i] = g_high_scores[i];
  }
  rec.best_lap_ticks = g_best_lap_ticks;
  write_all_chunks(fd, (char*)&rec, sizeof(struct ScoreDiskRecord));
  close(fd);
}

/* Save the current 128x128 3D terrain and biome to `path` (or default). */
bool world_save_file(char* path){
  char* target = path ? path : WORLD_FILE_PATH;
  int fd = open(target);
  if (fd < 0) return false;
  struct WorldDiskHeader hdr;
  hdr.magic = WORLD_MAGIC;
  hdr.biome = g_biome;
  hdr.sector = g_sector;
  hdr.seed = g_seed;
  bool ok = write_all_chunks(fd, (char*)&hdr, sizeof(struct WorldDiskHeader)) &&
            write_all_chunks(fd, (char*)g_terrain, MAP_CELLS * 2);
  close(fd);
  return ok;
}

/* Load a custom 128x128 3D terrain and biome from `path` (or default). */
bool world_load_file(char* path){
  char* target = path ? path : WORLD_FILE_PATH;
  int fd = open_existing(target);
  if (fd < 0) return false;
  struct WorldDiskHeader hdr;
  bool ok = read_all_chunks(fd, (char*)&hdr, sizeof(struct WorldDiskHeader)) &&
            hdr.magic == WORLD_MAGIC &&
            read_all_chunks(fd, (char*)g_terrain, MAP_CELLS * 2);
  close(fd);
  if (!ok) return false;
  if (hdr.biome >= 0 && hdr.biome < BIOME_COUNT) g_biome = hdr.biome;
  if (hdr.sector >= 1 && hdr.sector <= MAX_SECTORS) g_sector = hdr.sector;
  terrain_relight_rect(0, 0, MAP_SIZE - 1, MAP_SIZE - 1);
  return true;
}

#else
/* Host test stubs */
void scores_load(void){}
void scores_record(int score, int race_ticks){
  if (score > g_high_scores[0]) g_high_scores[0] = score;
  if (race_ticks > 0 && (g_best_lap_ticks == 0 || race_ticks < g_best_lap_ticks)){
    g_best_lap_ticks = race_ticks;
  }
}
bool world_save_file(char* path){ (void)path; return true; }
bool world_load_file(char* path){ (void)path; return true; }
#endif /* !SKYFORGE_HOST_TEST */
