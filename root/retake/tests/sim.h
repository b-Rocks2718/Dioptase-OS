#ifndef RETAKE_SIM_H
#define RETAKE_SIM_H
#include "../retake.h"
#include "../game.h"

/* Load a generated pack for native rule tests; reject malformed room tables. */
int sim_open(char* path);
/* Release the native pack after the final replay. */
void sim_close(void);
#endif
