#ifdef __SDCC
#pragma bank 255
#endif
#include "run.h"

void run_start(Run *r, uint8_t mode, uint16_t seed, uint16_t room) CORE_BANKED
{
    r->mode = mode;
    r->seed = seed;
    r->room = room ? room : 1;
    r->energy = RUN_START_ENERGY;
    r->streak = 0;
    r->best_streak = 0;
    r->bests = 0;
    r->cleared = 0;
    r->moves = 0;
    r->over = 0;
}

uint8_t run_on_move(Run *r) CORE_BANKED
{
    if (r->moves < 255) r->moves++;
    if (r->mode != MODE_RUN) return 0;
    if (r->energy) r->energy--;
    return 0;
}

uint8_t run_is_over(const Run *r) CORE_BANKED
{
    return (uint8_t)(r->mode == MODE_RUN && r->energy == 0);
}

uint8_t run_grade(uint8_t moves, uint8_t par) CORE_BANKED
{
    if (moves <= par) return GRADE_BEST;
    if (moves <= (uint8_t)(par + 2)) return GRADE_GOOD;
    return GRADE_OK;
}

uint8_t run_best_bonus(uint8_t streak) CORE_BANKED
{
    uint8_t extra = (uint8_t)(streak / RUN_STREAK_STEP);
    return (uint8_t)(RUN_BEST_BONUS + (extra < RUN_STREAK_MAX ? extra : RUN_STREAK_MAX));
}

uint8_t run_on_win(Run *r, uint8_t par) CORE_BANKED
{
    uint8_t gain = 0;
    uint16_t e;
    uint8_t g = run_grade(r->moves, par);

    if (g == GRADE_BEST) {
        r->bests++;
        if (r->streak < 255) r->streak++;
        if (r->streak > r->best_streak) r->best_streak = r->streak;
    } else {
        r->streak = 0;
    }
    if (r->mode == MODE_RUN) {
        gain = par;
        if (g == GRADE_BEST)
            gain = (uint8_t)(gain + run_best_bonus(r->streak));
        e = (uint16_t)r->energy + gain;
        if (e > RUN_MAX_ENERGY) e = RUN_MAX_ENERGY;
        gain = (uint8_t)(e - r->energy);
        r->energy = (uint8_t)e;
    }
    r->cleared++;
    r->room++;
    r->moves = 0;
    return gain;
}

uint8_t run_try_hint(Run *r) CORE_BANKED
{
    if (r->mode != MODE_RUN) return 1;
    if (r->energy <= RUN_HINT_COST) return 0;
    r->energy = (uint8_t)(r->energy - RUN_HINT_COST);
    r->streak = 0;
    return 1;
}

uint8_t run_skip(Run *r) CORE_BANKED
{
    if (r->mode != MODE_ZEN) return 0;
    r->room++;
    r->moves = 0;
    r->streak = 0;
    return 1;
}
