#ifndef AL_PLAY_H
#define AL_PLAY_H
#include <stdint.h>
#include <gb/gb.h>
#include "../core/run.h"

extern Run run;

/* how a run ended */
enum { PLAY_QUIT = 0, PLAY_OVER };

/* play stages from run.stage until the player quits or the run ends */
uint8_t play_run(void) BANKED;

#endif
