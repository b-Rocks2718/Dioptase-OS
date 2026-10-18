#ifndef RETAKE_SIM_H
#define RETAKE_SIM_H
#include "../retake.h"
#include "../game.h"

/* Load a generated pack for native rule tests; reject malformed room tables. */
int sim_open(char* path);
/* Release the native pack after the final replay. */
void sim_close(void);
/* Solver snapshots are native-only, opaque, and valid only for the loaded pack. */
int sim_snapshot_size(void);
/* Copy all mutable rule state; the caller supplies sim_snapshot_size bytes. */
void sim_snapshot(void* dst);
/* Restore a snapshot without entering the room again or resetting its actors. */
void sim_restore(void* src);
/* Export compact telemetry for route authoring; indices are documented in sim.c. */
void sim_state(int* out);
#endif
