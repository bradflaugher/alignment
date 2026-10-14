#ifndef AL_RUN_H
#define AL_RUN_H
#include <stdint.h>
#include "room.h"   /* CORE_BANKED */

/* Run-mode economy (BEST = OPTIMAL, a clear in exactly par turns): every move costs
 * one unit of energy. Rewind is free, but
 * spent energy never comes back ("time only flows forward for your battery").
 * Clearing a room refunds its par; a BEST (par-perfect) clear pays a bonus that
 * grows with your streak (+1, +2 from a 3-streak, +3 from a 6-streak). Hitting zero
 * ends the run. The cap is kept low on purpose: with a deep tank, a good player could
 * bank enough energy to never feel a mistake, and the run would never end. */
#define RUN_START_ENERGY 60
#define RUN_MAX_ENERGY   90
#define RUN_HINT_COST    3
#define RUN_BEST_BONUS   2
#define RUN_STREAK_STEP  3   /* +1 more bonus every this many BESTs in a row... */
#define RUN_STREAK_MAX   2   /* ...up to this much more */

enum { MODE_RUN = 0, MODE_ZEN };
enum { GRADE_OK = 0, GRADE_GOOD, GRADE_BEST };

typedef struct {
    uint8_t mode;
    uint16_t seed;
    uint16_t room;     /* current room, starts at 1 */
    uint8_t energy;
    uint8_t streak;      /* consecutive BEST clears */
    uint8_t best_streak;
    uint16_t bests;      /* total BEST clears */
    uint16_t cleared;
    uint8_t moves;       /* moves made in this room (including rewound ones) */
    uint8_t over;        /* run ended */
} Run;

void run_start(Run *r, uint8_t mode, uint16_t seed, uint16_t room) CORE_BANKED;
/* call after every real move; returns 1 if the run just ended (out of energy) */
uint8_t run_on_move(Run *r) CORE_BANKED;
/* grade of a clear using `moves` actually made vs par */
uint8_t run_grade(uint8_t moves, uint8_t par) CORE_BANKED;
/* energy a BEST pays on top of par, given the streak including this clear */
uint8_t run_best_bonus(uint8_t streak) CORE_BANKED;
/* call on win; returns energy gained (run mode) */
uint8_t run_on_win(Run *r, uint8_t par) CORE_BANKED;
/* returns 1 and charges if a hint is affordable */
uint8_t run_try_hint(Run *r) CORE_BANKED;
/* skip current room (zen only). returns 1 if allowed */
uint8_t run_skip(Run *r) CORE_BANKED;
/* in run mode: check if a stalled run is over (energy 0 with no win) */
uint8_t run_is_over(const Run *r) CORE_BANKED;

#endif
