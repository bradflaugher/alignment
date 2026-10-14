/* ALIGNMENT - background work: the room generator and hint searches run as a
 * coroutine in the CPU time the game doesn't use. */
#ifndef AL_BGJOB_H
#define AL_BGJOB_H
#include <stdint.h>
#include "../core/room.h"
#include "../core/gen.h"

extern Room bg_room;            /* the room the generator built */
extern Door bg_door;            /* ...and its door */
extern volatile uint8_t bg_gen_state;   /* 0 idle, 1 working, 2 done */
extern volatile uint8_t bg_hint_state;  /* 0 idle, 1 working, 2 done */
extern uint8_t bg_hint_act, bg_hint_rem; /* the answer: ACT_*, turns left (0 = no way out) */

void bg_init(void);
void bg_start_gen(uint16_t seed, const Door *d);
void bg_cancel_gen(void);       /* (forget the job; the coroutine finishes it unseen) */
void bg_request_hint(const Room *R, const State *s);
/* once per frame from the main loop: work until the frame's slice is used up */
void bg_run(void);
/* work flat out (the player is waiting), keeping VBlank, sound and fx alive */
void bg_run_frame(void);

#endif
