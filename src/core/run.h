/* ALIGNMENT - a run: lives, score, the stage you're on. */
#ifndef AL_RUN_H
#define AL_RUN_H
#include <stdint.h>
#include "stage.h"  /* CORE_BANKED */

#define RUN_LIVES 3
#define RUN_MAX_LIVES 9
#define RUN_CELLS_PER_LIFE 25   /* every 25 energy cells is an extra life */
#define SCORE_CELL 100
#define SCORE_STAGE 1000
#define SCORE_PERFECT 2000      /* every energy cell of a stage collected */
#define PAR_FRAMES_PER_COL 90   /* time bonus: par time is 1.5 s per column */

enum { MODE_RUN = 0, MODE_ZEN };

typedef struct {
    uint8_t mode;
    uint16_t seed;
    uint16_t stage;         /* from 1 */
    uint8_t lives;
    uint32_t score;
    uint16_t cells;         /* collected in the run */
    uint8_t stage_cells;    /* collected in this stage */
    uint16_t deaths;
    uint16_t stage_time;    /* frames in this stage */
    uint16_t cleared;
    uint8_t life_cells;     /* cells towards the next extra life */
} Run;

void run_start(Run *r, uint8_t mode, uint16_t seed, uint16_t stage) CORE_BANKED;
/* an energy cell: returns 1 if it gave an extra life */
uint8_t run_cell(Run *r) CORE_BANKED;
/* lost the mech: returns 1 if the run is over */
uint8_t run_death(Run *r) CORE_BANKED;
/* the exit: returns the bonus added (time + all cells) */
uint16_t run_clear(Run *r, uint8_t width, uint8_t ncells) CORE_BANKED;

#endif
