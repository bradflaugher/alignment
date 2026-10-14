#ifndef AL_SOLVER_H
#define AL_SOLVER_H
#include "room.h"

/* Breadth-first search over every reachable (mech, gravity, cells, crates, drones)
 * state. States are packed into 6 bytes and kept with their hash chains in an 8 KB
 * work area: cartridge RAM bank 1 on the Game Boy, a static array on the host. */
#define SOLVE_CAP 1024          /* states per search (6 + 2 bytes each = 8 KB) */
#define SOLVE_MAX_DEPTH 90

/* results of the last search */
extern uint16_t solve_visited;  /* states visited */
extern uint8_t solve_first;     /* first action of the solution found (ACT_*) */
extern uint8_t solve_flips;     /* flips in that solution */
extern uint8_t solve_capped;    /* the search hit SOLVE_CAP before it finished */
extern uint8_t solve_limit;     /* give up past this depth (SOLVE_MAX_DEPTH by default) */

/* Fewest turns from *from to the exit, 0 if there is no way out (or the search hit
 * the cap / depth limit). */
uint8_t solve(const Room *R, const State *from) CORE_BANKED;

/* Background searches (the generator runs as a coroutine on the Game Boy): solve_bg
 * calls solve_yield() between expansions. If another search ran in the meantime
 * (solve_epoch moved), it starts over, so the answer never depends on the timing. */
extern void (*solve_yield)(void);
extern uint8_t solve_epoch;
uint8_t solve_bg(const Room *R, const State *from) CORE_BANKED;

/* Explore without an exit: for every border cell that could be the exit, the fewest
 * turns to leave through it with every energy cell (0 = can't). Fills explore_depth[]
 * (border cells only) and explore_reach[] (bit per cell: the mech can stand there).
 * Runs in the background like solve_bg. Returns the number of states visited. */
extern uint8_t explore_depth[RN];
extern uint8_t explore_flips[RN];     /* flips on the way out through that cell */
extern uint8_t explore_reach[(RN + 7) / 8];
uint16_t explore_bg(const Room *R, const State *from) CORE_BANKED;

#ifndef __SDCC
extern uint8_t solver_mem[8192];
#endif

#endif
