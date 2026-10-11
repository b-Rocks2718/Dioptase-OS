/* Fixed-width high-score table with manual decimal parsing. */

#include "save.h"

#include "../crt/fcntl.h"
#include "../crt/unistd.h"

#define SCORE_PATH "/emberfall/highscores.txt"
#define ENTRY_BYTES 28

static void zero_scores(struct HighScores* hs) {
  int i = 0;
  while (i < 5) {
    hs->score[i] = 0;
    hs->time_sec[i] = 0;
    hs->kills[i] = 0;
    hs->level[i] = 0;
    i++;
  }
}

static int parse_digits(char* buf, int off, int len) {
  int v = 0;
  int i = 0;
  while (i < len) {
    char c = buf[off + i];
    if (c < '0' || c > '9') return v;
    v = v * 10 + (c - '0');
    i++;
  }
  return v;
}

static void write_digits(char* buf, int off, int len, int v) {
  int i = len - 1;
  if (v < 0) v = 0;
  while (i >= 0) {
    buf[off + i] = '0' + (v % 10);
    v /= 10;
    i--;
  }
}

/* Read the fixed-width table, zeroing on failure. */
void save_load(struct HighScores* hs) {
  int fd = 0;
  char buf[140];
  int n = 0;
  int i = 0;
  zero_scores(hs);
  fd = open_existing(SCORE_PATH);
  if (fd < 0) return;
  n = read(fd, buf, 140);
  close(fd);
  if (n < 140) return;
  while (i < 5) {
    int off = i * ENTRY_BYTES;
    hs->score[i] = parse_digits(buf, off, 10);
    hs->time_sec[i] = parse_digits(buf, off + 11, 6);
    hs->kills[i] = parse_digits(buf, off + 18, 6);
    hs->level[i] = parse_digits(buf, off + 25, 2);
    i++;
  }
}

/* Insert a run in rank order and persist the table. */
int save_record(struct HighScores* hs, int score, int time_sec, int kills,
    int level) {
  int pos = 5;
  int i = 0;
  int fd = 0;
  char buf[140];
  while (i < 5) {
    if (score > hs->score[i]) {
      pos = i;
      break;
    }
    i++;
  }
  if (pos >= 5) return 0;
  i = 4;
  while (i > pos) {
    hs->score[i] = hs->score[i - 1];
    hs->time_sec[i] = hs->time_sec[i - 1];
    hs->kills[i] = hs->kills[i - 1];
    hs->level[i] = hs->level[i - 1];
    i--;
  }
  hs->score[pos] = score;
  hs->time_sec[pos] = time_sec;
  hs->kills[pos] = kills;
  hs->level[pos] = level;
  i = 0;
  while (i < 5) {
    int off = i * ENTRY_BYTES;
    write_digits(buf, off, 10, hs->score[i]);
    buf[off + 10] = ' ';
    write_digits(buf, off + 11, 6, hs->time_sec[i]);
    buf[off + 17] = ' ';
    write_digits(buf, off + 18, 6, hs->kills[i]);
    buf[off + 24] = ' ';
    write_digits(buf, off + 25, 2, hs->level[i]);
    buf[off + 27] = '\n';
    i++;
  }
  fd = open(SCORE_PATH);
  if (fd < 0) return pos == 0;
  write(fd, buf, 140);
  close(fd);
  return pos == 0;
}
