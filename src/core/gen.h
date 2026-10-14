#ifndef AL_GEN_H
#define AL_GEN_H
#include "room.h"

/* room where each mechanic is introduced */
#define UNLOCK_PLASMA 3
#define UNLOCK_PICK   5
#define UNLOCK_CRATE  8
#define UNLOCK_BOT   12
#define UNLOCK_PHASE 16
#define UNLOCK_DOOR  20
#define FREEFORM_ROOM 24
#define DECK_ROOMS 8            /* rooms per deck: a new name, palette and song */

/* Work is counted in states visited plus a per-search overhead, so the host and the
 * Game Boy stop at exactly the same point. */
#define GEN_SEARCH_OVERHEAD 12
#define GEN_BUDGET 3000
#define GEN_TIDY_BUDGET 3800

typedef struct {
    uint8_t mechs;      /* mechanics placed */
    uint8_t featured;   /* introduced here (0xFF none) */
    uint8_t focus;      /* must change par (0xFF none) */
    uint8_t par_min, par_max;
    uint8_t flips_min;
    uint8_t picks, crates, bots;
    uint8_t walls;      /* structure pieces */
} GenParams;

/* where a room is entered */
typedef struct {
    uint16_t index;     /* room number, from 1 */
    uint8_t side;       /* SIDE_L / SIDE_T / SIDE_B */
    uint8_t cell;       /* the opening */
    int16_t rx, ry;     /* the room's place on the station map */
} Door;

extern uint16_t gen_cost;     /* work spent on the last room */
extern uint16_t gen_searches; /* searches run for the last room */
extern uint8_t gen_score;     /* how far the last room missed its window (0 = inside) */

void gen_first_door(Door *d) CORE_BANKED;
/* the door into the next room, from this room's exit */
void gen_next_door(const Door *d, const Room *R, Door *next) CORE_BANKED;
uint8_t gen_new_mech(uint16_t index) CORE_BANKED;
void gen_params(uint16_t index, GenParams *p) CORE_BANKED;
/* Build room d->index of run `seed`. Always solvable (R->par > 0). On the Game Boy it
 * runs as a coroutine: the searches yield through solve_yield. */
void gen_room(Room *R, uint16_t seed, const Door *d) CORE_BANKED;

#endif
