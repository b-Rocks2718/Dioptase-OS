#ifndef RETAKE_SAVE_H
#define RETAKE_SAVE_H
#include "game.h"

/* Write the older of two recoverable slots, preserving the last valid one.
 * Single user-process owner; no concurrent writers. Returns 0 on verified
 * success, -1 on failure. The caller must not display success on failure. */
int progress_save(struct Progress* progress);

/* Decode the newest compatible valid slot; leave output unchanged on failure.
 * Validation includes checkpoint geometry and all bounded progress fields. */
int progress_load(struct Progress* output);

/* Describe the last failed operation for UI/logging; static storage. */
char* progress_error(void);
#endif
