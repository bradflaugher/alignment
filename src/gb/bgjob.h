/* ALIGNMENT - background work: the room generator and hint searches run as a
 * coroutine in the CPU time the game doesn't use. The generator works up to
 * BG_AHEAD rooms ahead of the player (a room's door only depends on the room
 * before it), so a slow room hides behind quick ones. */
#ifndef AL_BGJOB_H
#define AL_BGJOB_H
#include <stdint.h>
#include "../core/room.h"
#include "../core/gen.h"

#define BG_AHEAD 3

extern volatile uint8_t bg_hint_state;  /* 0 idle, 1 working, 2 done */
extern uint8_t bg_hint_act, bg_hint_rem; /* the answer: ACT_*, turns left (0 = no way out) */

void bg_init(void);
/* forget everything queued; start building from door d of run seed */
void bg_reset(uint16_t seed, const Door *d);
/* rooms built and waiting */
uint8_t bg_ready(void);
/* take the next room (bg_ready() must be > 0) */
void bg_take(Room *R, Door *d);
/* work units the generator has done on the room it is building (for a progress bar) */
uint16_t bg_progress(void);
void bg_request_hint(const Room *R, const State *s);
/* once per frame from the main loop: work until the frame's slice is used up */
void bg_run(void);

#endif
