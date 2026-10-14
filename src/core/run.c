#ifdef __SDCC
#pragma bank 255
#endif
#include "run.h"

void run_start(Run *r, uint8_t mode, uint16_t seed, uint16_t stage) CORE_BANKED
{
    r->mode = mode;
    r->seed = seed;
    r->stage = stage ? stage : 1;
    r->lives = RUN_LIVES;
    r->score = 0;
    r->cells = 0;
    r->stage_cells = 0;
    r->deaths = 0;
    r->stage_time = 0;
    r->cleared = 0;
    r->life_cells = 0;
}

uint8_t run_cell(Run *r) CORE_BANKED
{
    r->score += SCORE_CELL;
    r->cells++;
    r->stage_cells++;
    if (++r->life_cells >= RUN_CELLS_PER_LIFE) {
        r->life_cells = 0;
        if (r->lives < RUN_MAX_LIVES) {
            r->lives++;
            return 1;
        }
    }
    return 0;
}

void run_score(Run *r, uint16_t points) CORE_BANKED
{
    r->score += points;
}

uint8_t run_death(Run *r) CORE_BANKED
{
    r->deaths++;
    if (r->lives) r->lives--;
    return (uint8_t)(r->lives == 0);
}

uint16_t run_clear(Run *r, uint8_t width, uint8_t ncells) CORE_BANKED
{
    uint16_t bonus = SCORE_STAGE, par = (uint16_t)width * PAR_FRAMES_PER_COL;
    if (r->stage_time < par) bonus += (uint16_t)((par - r->stage_time) / 6);   /* 10 a second */
    if (ncells && r->stage_cells >= ncells) bonus += SCORE_PERFECT;
    r->score += bonus;
    r->cleared++;
    r->stage++;
    r->stage_cells = 0;
    r->stage_time = 0;
    return bonus;
}
