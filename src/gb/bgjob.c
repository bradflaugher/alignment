/* ALIGNMENT - the background coroutine (see coro.s). Bank 0.
 *
 * The coroutine loops over two kinds of job: build a room (gen_room) and find a
 * hint (solve_bg). Searches call bg_yield between expansions; it hands the CPU back
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

Room bg_room;
Door bg_door;
volatile uint8_t bg_gen_state, bg_hint_state;
uint8_t bg_hint_act, bg_hint_rem;

static uint16_t job_seed;
static Door job_door;
static volatile uint8_t gen_req, hint_req, in_hint;
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
    for (;;) {
        if (hint_req) {
            do_hint();
            continue;
        }
        if (gen_req) {
            gen_req = 0;
            gen_room(&bg_room, job_seed, &job_door);
            if (!gen_req) {
                memcpy(&bg_door, &job_door, sizeof(Door));
                bg_gen_state = 2;
            }
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
    ly_stop = is_cgb ? 128 : 104;
    bg_gen_state = 0;
    bg_hint_state = 0;
}

void bg_start_gen(uint16_t seed, const Door *d)
{
    job_seed = seed;
    memcpy(&job_door, d, sizeof(Door));
    bg_gen_state = 1;
    gen_req = 1;        /* a build in progress restarts from this request when it ends */
}

void bg_cancel_gen(void)
{
    bg_gen_state = 0;
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
    if (bg_gen_state != 1 && bg_hint_state != 1) return;
    slice_frame = frame;
    coro_resume();
}

void bg_run_frame(void)
{
    wait_vbl_done();
    fx_update();
    bg_run();
}
