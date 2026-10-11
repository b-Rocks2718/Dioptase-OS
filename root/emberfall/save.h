/* Emberfall persistence: top-five high scores in a fixed-width file.
 *
 * Each entry is 28 bytes: 10-digit score, space, 6-digit time, space,
 * 6-digit kills, space, 2-digit level, newline. Five entries total.
 */

#ifndef EMBER_SAVE_H
#define EMBER_SAVE_H

#include "render.h"

/* Load rankings, zeroing on any failure. */
void save_load(struct HighScores* hs);

/* Insert a run, keeping the table sorted, and write it back. */
int save_record(struct HighScores* hs, int score, int time_sec, int kills,
  int level);

#endif
