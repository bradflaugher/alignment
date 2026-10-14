#ifndef AL_PLAY_H
#define AL_PLAY_H
#include <stdint.h>
#include "../core/run.h"
#include "../core/gen.h"

extern Run run;

/* how a run ended */
enum { PLAY_QUIT = 0, PLAY_OVER };

/* play from door d of run seed run.seed until the player quits or the run ends */
uint8_t play_run(const Door *d) BANKED;
const char *deck_name(uint16_t index) BANKED;

/* the station map: the last rooms visited */
#define MAP_N 48
extern int16_t map_rx[MAP_N], map_ry[MAP_N];
extern uint8_t map_n, map_head;

#endif
