/* ALIGNMENT - portable core: a stage of the station, its generator and the
 * reachability check. Compiles with SDCC (GBDK) and host gcc. */
#ifndef AL_STAGE_H
#define AL_STAGE_H
#include <stdint.h>

#if defined(__SDCC)
#define CORE_BANKED __banked
#else
#define CORE_BANKED
#endif

/* A stage is SW x SH cells of 16x16 px: up to 128 cells (12 screens) wide and 16 tall
 * (the height of the Game Boy's background map, so only columns ever stream in). */
#define SH 16
#define SW_MAX 128
#define SCELL(x, y) ((uint16_t)(x) * SH + (y))      /* column-major: a column is 16 bytes */

/* terrain */
enum {
    T_EMPTY = 0,
    T_WALL,
    T_PLASMA,     /* deadly to touch */
    T_PHASE_P,    /* purple polarity block: solid while gravity is down */
    T_PHASE_G,    /* green polarity block: solid while gravity is up */
    T_CELL,       /* an energy cell to collect */
    T_CHECK,      /* a checkpoint beacon (not solid) */
    T_EXIT,       /* the exit gate (not solid): touch it to clear the stage */
    T_LASER,      /* a laser gate: deadly while it is on (it blinks on a fixed rhythm) */
    T_GIRDER,     /* a background girder: decoration, not solid */
    NUM_T
};

/* gravity */
#define G_DOWN 0
#define G_UP 1

/* enemies: all move as pure functions of time, so they need no state */
enum {
    E_CRAWLER = 0,  /* walks a floor (or a ceiling) back and forth between x0 and x1 */
    E_HOVER,        /* floats up and down between y0 and y1 in its column */
    E_CRUSHER,      /* a piston that slams from the ceiling to the floor and back */
    NUM_E
};

#define MAX_ENEMIES 24
#define MAX_CELLS 48

typedef struct {
    uint8_t kind;
    uint8_t x0, x1;     /* cells (crawler range; hover/crusher: x0 = its column) */
    uint8_t y0, y1;     /* cells (crawler: y0 = its row, y1 = 1 if it hangs from a ceiling) */
    uint8_t speed;      /* 1..4 */
    uint8_t phase;      /* time offset */
} Enemy;

/* mechanics, in unlock order */
enum { M_PITS = 0, M_PLASMA, M_STAGGER, M_CRAWLER, M_PHASE, M_HOVER, M_LASER, M_CRUSHER,
       M_SHAFT, NUM_MECH };

typedef struct {
    uint8_t cell[SW_MAX * SH];  /* terrain, column-major */
    uint8_t w;                  /* width in cells */
    uint8_t start_x, start_y;   /* where the mech appears (standing on a floor, gravity down) */
    uint8_t check_x, check_y;   /* the checkpoint beacon */
    uint8_t exit_x, exit_y;     /* the exit gate */
    uint8_t nenemies;
    Enemy enemy[MAX_ENEMIES];
    uint8_t ncells;             /* energy cells placed */
    uint8_t deck;               /* look & music theme */
    uint8_t fixes;              /* generator: sections it had to make easier */
    uint16_t index;             /* stage number, from 1 */
    uint16_t mechs;             /* MBIT(M_*) used */
    uint8_t featured;           /* mechanic introduced on this stage, or 0xFF */
} Stage;
#define MBIT(m) ((uint16_t)(1u << (m)))

#define STAGE_AT(S, x, y) ((S)->cell[SCELL((x), (y))])

/* stage n of run `seed` (always passable: checked, and repaired if needed) */
void stage_gen(Stage *S, uint16_t seed, uint16_t index) CORE_BANKED;
uint8_t stage_new_mech(uint16_t index) CORE_BANKED;

/* Reachability with conservative movement rules (walk, climb a one-cell step, jump a
 * gap of up to two cells, drop, flip gravity and fall straight across). Returns 1 if the
 * exit can be reached from the start. Hazards that move (enemies, lasers) are not walls:
 * they are timing obstacles with safe windows, by construction. */
uint8_t stage_check(const Stage *S) CORE_BANKED;
/* cells the checker reached (bit per cell, column-major), valid after stage_check */
extern uint8_t check_seen[SW_MAX * SH * 2 / 8];

#endif
