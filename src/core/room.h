/* ALIGNMENT - portable puzzle core: the room, its rules and the turn simulation.
 * Compiles with SDCC (GBDK) and host gcc. Keep types explicit: SDCC int is 16 bit. */
#ifndef AL_ROOM_H
#define AL_ROOM_H
#include <stdint.h>

/* The generator and solver live in switchable ROM banks on the Game Boy. */
#if defined(__SDCC)
#define CORE_BANKED __banked
#else
#define CORE_BANKED
#endif

/* A room is the whole screen: 10x9 cells of 16x16 px. Cell index = y*10 + x.
 * Row 0, row 8, column 0 and column 9 are the border: wall, except the entry and
 * the exit openings. Corners are always wall. No cell index reaches 128, so a cell
 * fits in 7 bits and 0 (a corner) doubles as "no object". */
#define RW 10
#define RH 9
#define RN 90
#define CELL(x, y) ((uint8_t)((y) * RW + (x)))
#define CX(c) (cell_x[(c)])
#define CY(c) (cell_y[(c)])
#define NONE 0

#define MAX_PICKS 3
#define MAX_CRATES 2
#define MAX_BOTS 2
#define NOBJ (1 + MAX_CRATES + MAX_BOTS)   /* object slots: 0 mech, 1-2 crates, 3-4 drones */
#define SLOT_CRATE 1
#define SLOT_BOT 3

/* terrain */
enum {
    T_EMPTY = 0,
    T_WALL,
    T_PLASMA,     /* kills the mech and drones that enter it; crates don't care */
    T_PHASE_P,    /* purple polarity block: solid while gravity is down */
    T_PHASE_G,    /* green polarity block: solid while gravity is up */
    T_PLATE,      /* pressure plate: pressed while anything occupies it */
    T_DOOR,       /* laser door: solid unless a plate is pressed */
    T_PICK0,      /* energy cells 0..2: collect them all to open the exit */
    T_PICK1,
    T_PICK2,
    T_ENTRY,      /* the opening the mech came in through (solid to crates and drones) */
    T_EXIT,       /* the way out: sealed by a force field until every cell is collected */
    NUM_T
};
#define IS_PICK(t) ((uint8_t)((t) - T_PICK0) < 3)

/* sides of the room */
enum { SIDE_L = 0, SIDE_R, SIDE_T, SIDE_B };

/* gravity */
#define G_DOWN 0
#define G_UP 1

/* actions */
enum { ACT_L = 0, ACT_R, ACT_FLIP, NUM_ACT };

/* mechanics, in unlock order (bit ids) */
enum { M_PLASMA = 0, M_PICK, M_CRATE, M_BOT, M_PHASE, M_DOOR, NUM_MECH };
#define MBIT(m) ((uint8_t)(1u << (m)))

/* turn results */
enum { TR_BLOCKED = 0, TR_OK, TR_WIN, TR_DEAD };

/* what destroyed the mech */
enum { DIE_NONE = 0, DIE_ZAP, DIE_CRUSH, DIE_BOT };

typedef struct {
    uint8_t cell[RN];       /* terrain T_* */
    uint8_t entry;          /* entry opening cell */
    uint8_t entry_side;     /* SIDE_L / SIDE_T / SIDE_B */
    uint8_t exit;           /* exit opening cell */
    uint8_t exit_side;      /* SIDE_R / SIDE_T / SIDE_B */
    uint8_t g0;             /* gravity at the start */
    uint8_t npicks;
    uint8_t crate0[MAX_CRATES];   /* starting cells (NONE = unused) */
    uint8_t bot0[MAX_BOTS];
    uint8_t par;            /* fewest turns out, 0 = unsolved */
    uint8_t flips;          /* flips in the solution the solver found */
    uint8_t mechs;          /* MBIT(M_*) present */
    uint8_t featured;       /* mechanic introduced in this room, or 0xFF */
    uint8_t fallback;       /* 1 = the hand-made safety room */
    uint16_t index;         /* room number, from 1 */
} Room;

typedef struct {
    uint8_t o[NOBJ];        /* object cells: 0 mech, 1-2 crates, 3-4 drones; NONE = absent */
    uint8_t g;              /* G_DOWN / G_UP */
    uint8_t got;            /* collected energy cells, bit i = T_PICKi */
} State;

/* What happened in one turn, for the animation. Phase 1 is the action (step, push or
 * flip), phase 2 the drones' step, phase 3 the fall (every object falls at once). */
#define EV_FLIP       0x01
#define EV_PUSH       0x02
#define EV_EXIT_OPEN  0x04   /* the last energy cell came in this turn */
#define EV_DOOR       0x08   /* the doors changed state */
#define EV_WIN        0x10
#define EV_DEAD       0x20
#define EV_BOT_DIED   0x40
#define EV_PICK       0x80
#define NO_KILLER 0xFF
typedef struct {
    uint8_t flags;
    uint8_t act;
    uint8_t start[NOBJ];    /* cells before the turn */
    uint8_t step[NOBJ];     /* cells after the action + drone phase (before the fall) */
    uint8_t end[NOBJ];      /* cells after the fall; NONE for a destroyed drone... */
    uint8_t died_at[NOBJ];  /* ...which was destroyed here (for the mech: where it died) */
    uint8_t killer[NOBJ];   /* slot of the object that crushed it, or NO_KILLER */
    uint8_t cause;          /* DIE_* for the mech */
    uint8_t picked;         /* energy cells collected this turn (mask) */
    uint8_t doors;          /* doors open after the turn (1/0) */
} TurnLog;

extern const uint8_t cell_x[RN];
extern const uint8_t cell_y[RN];

void room_clear(Room *R);
/* the room's starting state: everything placed, then fallen into place (the mech
 * drops in from a ceiling or floor doorway). TR_DEAD if that kills it (never for a
 * generated room). */
uint8_t state_start(const Room *R, State *s);
/* mask of every energy cell in the room */
uint8_t room_full(const Room *R);
/* One turn. Updates *s unless the action is blocked (TR_BLOCKED leaves *s alone and
 * costs no turn). On TR_DEAD *s holds the moment of death. log may be NULL. */
uint8_t sim_turn(const Room *R, State *s, uint8_t act, TurnLog *log);
/* doors open in this state (a plate is held down) */
uint8_t doors_open(const Room *R, const State *s);
/* let every object fall into place under gravity s->g (no turn, no drone step);
 * returns TR_OK or TR_DEAD (a drone fell into plasma counts as fine; it is removed) */
uint8_t settle_state(const Room *R, State *s);

/* The turn simulation runs on file-scope globals (SDCC is slow with struct pointers).
 * sim_load(R) once per room; f_o/f_g/f_got hold the state; sim_core(act, log). */
extern uint8_t f_o[NOBJ];
extern uint8_t f_g, f_got;
void sim_load(const Room *R);
uint8_t sim_core(uint8_t act, TurnLog *log);

#endif
