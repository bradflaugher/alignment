/* ALIGNMENT - the background coroutine (see coro.s). Bank 0.
 *
 * The coroutine loops over two kinds of job: build the next room (gen_room) and find
 * a hint (solve_bg). Searches call bg_yield between expansions; it hands the CPU back
 * to the game once the frame's slice is spent (the LCD has drawn far enough down the
 * screen, or a VBlank already came). A hint asked for in the middle of a room build
 * runs right there, nested inside that build's yield; the build's own search then
 * notices the work area was used and restarts, so its result never changes. */
#include <gb/gb.h>
#include <string.h>
#include "bgjob.h"
#include "gfx.h"
#include "../core/solver.h"

void coro_resume(void);
void coro_yield(void);
extern uint16_t coro_gen_sp;
extern uint8_t coro_gen_bank;

#define STACK_SIZE 1024
static uint8_t stack[STACK_SIZE];

volatile uint8_t bg_hint_state;
uint8_t bg_hint_act, bg_hint_rem;

/* the queue of built rooms */
static Room q_room[BG_AHEAD];
static Door q_door[BG_AHEAD];
static uint8_t q_head, q_n;
static Room work_room;          /* being built */
static Door work_door;
static Door next_door;          /* the next room to build */
static uint16_t seed;
static uint8_t epoch;           /* bumped by bg_reset: a build in flight is dropped */
static uint8_t active;          /* a run is going: keep building */

static volatile uint8_t hint_req, in_hint;
static Room hint_room;
static State hint_from;
static uint8_t slice_frame, ly_stop;

static uint8_t slice_over(void)
{
    uint8_t ly = LY_REG;
    if (frame != slice_frame) return 1;
    return (uint8_t)(ly >= ly_stop && ly < 144);
}

static void do_hint(void)
{
    uint8_t r;
    in_hint = 1;
    hint_req = 0;
    r = solve_bg(&hint_room, &hint_from);
    bg_hint_act = solve_first;
    bg_hint_rem = r;
    if (!hint_req) bg_hint_state = 2;
    in_hint = 0;
}

static void bg_yield(void)
{
    if (!slice_over()) return;
    coro_yield();
    if (hint_req && !in_hint) do_hint();
}

static void entry(void)
{
    uint8_t e, slot;
    for (;;) {
        if (hint_req) {
            do_hint();
            continue;
        }
        if (active && q_n < BG_AHEAD) {
            e = epoch;
            memcpy(&work_door, &next_door, sizeof(Door));
            gen_room(&work_room, seed, &work_door);
            if (e != epoch) continue;       /* reset while building: throw it away */
            slot = (uint8_t)((q_head + q_n) % BG_AHEAD);
            memcpy(&q_room[slot], &work_room, sizeof(Room));
            memcpy(&q_door[slot], &work_door, sizeof(Door));
            q_n++;
            gen_next_door(&work_door, &work_room, &next_door);
            continue;
        }
        coro_yield();
    }
}

void bg_init(void)
{
    uint16_t *sp = (uint16_t *)(stack + STACK_SIZE);
    *--sp = (uint16_t)entry;    /* coro_resume's ret lands here */
    *--sp = 0;                  /* bc */
    *--sp = 0;                  /* de */
    *--sp = 0;                  /* hl */
    coro_gen_sp = (uint16_t)sp;
    coro_gen_bank = 1;
    solve_yield = bg_yield;
    ly_stop = is_cgb ? 128 : 112;
    bg_hint_state = 0;
}

void bg_reset(uint16_t s, const Door *d)
{
    seed = s;
    memcpy(&next_door, d, sizeof(Door));
    q_head = 0;
    q_n = 0;
    epoch++;
    active = 1;
}

uint8_t bg_ready(void)
{
    return q_n;
}

void bg_take(Room *R, Door *d)
{
    memcpy(R, &q_room[q_head], sizeof(Room));
    memcpy(d, &q_door[q_head], sizeof(Door));
    q_head = (uint8_t)((q_head + 1) % BG_AHEAD);
    q_n--;
}

uint16_t bg_progress(void)
{
    return gen_cost;
}

void bg_request_hint(const Room *R, const State *s)
{
    memcpy(&hint_room, R, sizeof(Room));
    memcpy(&hint_from, s, sizeof(State));
    bg_hint_state = 1;
    hint_req = 1;
}

void bg_run(void)
{
    if (!(active && q_n < BG_AHEAD) && bg_hint_state != 1) return;
    slice_frame = frame;
    coro_resume();
}
