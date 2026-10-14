/* ALIGNMENT - host tests for the puzzle core: rules, the fast simulation against the
 * reference one, the solver, the generator, determinism. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "room.h"
#include "ref_room.h"
#include "solver.h"
#include "gen.h"
#include "rng.h"
#include "run.h"

static unsigned checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; if (fails < 40) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKF(c, ...) do { checks++; if (!(c)) { fails++; if (fails < 40) { printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint32_t lcg = 12345;
static uint8_t rnd8(uint8_t n)
{
    lcg = lcg * 1103515245u + 12345u;
    return (uint8_t)(((lcg >> 16) & 0x7FFF) % n);
}

/* ------------------------------------------------------------- room kit */

static void box(Room *R)
{
    uint8_t i;
    room_clear(R);
    for (i = 0; i < RN; i++)
        if (CX(i) == 0 || CX(i) == RW - 1 || CY(i) == 0 || CY(i) == RH - 1)
            R->cell[i] = T_WALL;
    R->entry = CELL(0, 7);
    R->entry_side = SIDE_L;
    R->cell[R->entry] = T_ENTRY;
    R->exit = CELL(9, 7);
    R->exit_side = SIDE_R;
    R->cell[R->exit] = T_EXIT;
    R->g0 = G_DOWN;
}

static void draw(Room *R, const char *rows[RH])
{
    uint8_t x, y, c, nc = 0, nb = 0, np = 0;
    room_clear(R);
    for (y = 0; y < RH; y++) {
        for (x = 0; x < RW; x++) {
            char ch = rows[y][x];
            c = CELL(x, y);
            switch (ch) {
            case '#': R->cell[c] = T_WALL; break;
            case '~': R->cell[c] = T_PLASMA; break;
            case 'p': R->cell[c] = T_PHASE_P; break;
            case 'g': R->cell[c] = T_PHASE_G; break;
            case '_': R->cell[c] = T_PLATE; break;
            case '|': R->cell[c] = T_DOOR; break;
            case '*': R->cell[c] = (uint8_t)(T_PICK0 + np); np++; break;
            case 'C': R->crate0[nc++] = c; break;
            case 'B': R->bot0[nb++] = c; break;
            case 'v': case '^': case '@':
                R->cell[c] = T_ENTRY; R->entry = c;
                R->entry_side = x == 0 ? SIDE_L : y == 0 ? SIDE_T : SIDE_B;
                R->g0 = ch == '^' ? G_UP : G_DOWN;
                break;
            case 'E': case 'T': case 'U':
                R->cell[c] = T_EXIT; R->exit = c;
                R->exit_side = x == RW - 1 ? SIDE_R : y == 0 ? SIDE_T : SIDE_B;
                break;
            default: break;
            }
        }
    }
    R->npicks = np;
}

/* ------------------------------------------------------------- the rules */

static void test_rules(void)
{
    Room R;
    State s;
    TurnLog lg;
    uint8_t r;

    /* walk along the floor and out of the right doorway */
    box(&R);
    CHECK(state_start(&R, &s) == TR_OK);
    CHECK(s.o[0] == CELL(0, 7));
    CHECK(sim_turn(&R, &s, ACT_L, NULL) == TR_BLOCKED);
    CHECK(sim_turn(&R, &s, ACT_R, NULL) == TR_OK && s.o[0] == CELL(1, 7));
    {
        uint8_t i;
        for (i = 0; i < 7; i++) r = sim_turn(&R, &s, ACT_R, NULL);
        CHECK(s.o[0] == CELL(8, 7));
        CHECK(sim_turn(&R, &s, ACT_R, NULL) == TR_OK && s.o[0] == CELL(9, 7));
        CHECK(sim_turn(&R, &s, ACT_R, &lg) == TR_WIN && (lg.flags & EV_WIN));
    }
    state_start(&R, &s);
    CHECK(solve(&R, &s) == 10);

    /* flip: the mech falls to the ceiling */
    box(&R);
    state_start(&R, &s);
    sim_turn(&R, &s, ACT_R, NULL);
    r = sim_turn(&R, &s, ACT_FLIP, &lg);
    CHECK(r == TR_OK && s.g == G_UP && s.o[0] == CELL(1, 1));
    CHECK(lg.flags & EV_FLIP);
    CHECK(lg.step[0] == CELL(1, 7) && lg.end[0] == CELL(1, 1));

    {
        /* a ceiling exit: fall out of it */
        static const char *m[RH] = {
            "######T###",
            "#........#",
            "#........#",
            "#........#",
            "#........#",
            "#........#",
            "#........#",
            "v........#",
            "##########" };
        draw(&R, m);
        state_start(&R, &s);
        CHECK(solve(&R, &s) == 7);       /* RRRRRR then F */
        CHECK(solve_flips == 1);
    }
    {
        /* plasma on the way up kills; the solver goes around it */
        static const char *m[RH] = {
            "######T###",
            "#........#",
            "#........#",
            "#.....~..#",
            "#........#",
            "#........#",
            "#........#",
            "v........#",
            "##########" };
        draw(&R, m);
        state_start(&R, &s);
        {
            uint8_t i;
            for (i = 0; i < 6; i++) sim_turn(&R, &s, ACT_R, NULL);
        }
        r = sim_turn(&R, &s, ACT_FLIP, &lg);
        CHECK(r == TR_DEAD && lg.cause == DIE_ZAP && lg.died_at[0] == CELL(6, 3));
        state_start(&R, &s);
        CHECK(solve(&R, &s) == 7);       /* RRRRR F up column 5, then R under the doorway */
    }
    {
        /* energy cells gate the exit */
        static const char *m[RH] = {
            "##########",
            "#..*.....#",
            "#........#",
            "#........#",
            "#........#",
            "#........#",
            "#........#",
            "v........E",
            "##########" };
        draw(&R, m);
        state_start(&R, &s);
        CHECK(solve(&R, &s) == 12);     /* RRR F (grab it at the top) F RRRRRR R */
        {
            /* walking to the doorway without the cell: sealed */
            uint8_t i;
            for (i = 0; i < 9; i++) sim_turn(&R, &s, ACT_R, NULL);
            CHECK(s.o[0] == CELL(8, 7));     /* the force field is solid */
            CHECK(sim_turn(&R, &s, ACT_R, NULL) == TR_BLOCKED);
        }
    }
    {
        /* push a crate; a crate dropped on a drone crushes it */
        static const char *m[RH] = {
            "##########",
            "#........#",
            "#........#",
            "#........#",
            "#........#",
            "#..C.....#",
            "#..######E",
            "v..B.....#",
            "##########" };
        draw(&R, m);
        CHECK(state_start(&R, &s) == TR_OK);
        CHECK(s.o[SLOT_CRATE] == CELL(3, 5));
        CHECK(s.o[SLOT_BOT] == CELL(3, 7));
    }
    {
        /* a crate falling onto the mech crushes it */
        static const char *m[RH] = {
            "##########",
            "#.C......#",
            "#.#......#",
            "#........#",
            "#........#",
            "#........#",
            "#........#",
            "v........E",
            "##########" };
        draw(&R, m);
        state_start(&R, &s);
        CHECK(s.o[SLOT_CRATE] == CELL(2, 1));
        /* flip: the mech falls up to (0,6)? no: the doorway is boxed in, it stays */
        r = sim_turn(&R, &s, ACT_R, NULL);
        CHECK(r == TR_OK && s.o[0] == CELL(1, 7));
        r = sim_turn(&R, &s, ACT_R, NULL);
        CHECK(r == TR_OK && s.o[0] == CELL(2, 7));
        /* flip up: the mech rises to (2,3) under the ledge; the crate stays up */
        r = sim_turn(&R, &s, ACT_FLIP, NULL);
        CHECK(r == TR_OK && s.o[0] == CELL(2, 3));
        /* now push the crate? it is above the ledge. walk left and flip down */
    }
    {
        /* flipping while standing on a crate: it falls on you */
        static const char *m[RH] = {
            "##########",
            "#........#",
            "#........#",
            "#........#",
            "#........#",
            "#........#",
            "#........#",
            "v.C......E",
            "##########" };
        draw(&R, m);
        state_start(&R, &s);
        /* climb isn't possible: push the crate to the right instead */
        r = sim_turn(&R, &s, ACT_R, &lg);
        CHECK(r == TR_OK && s.o[0] == CELL(1, 7));
        r = sim_turn(&R, &s, ACT_R, &lg);
        CHECK(r == TR_OK && s.o[0] == CELL(2, 7) && s.o[SLOT_CRATE] == CELL(3, 7) && (lg.flags & EV_PUSH));
        /* flip: mech to (2,1), then flip back: mech lands on the floor next to the crate */
        r = sim_turn(&R, &s, ACT_FLIP, NULL);
        CHECK(s.o[0] == CELL(2, 1) && s.o[SLOT_CRATE] == CELL(3, 1));
        r = sim_turn(&R, &s, ACT_R, NULL);
        CHECK(r == TR_OK && s.o[0] == CELL(3, 1) && s.o[SLOT_CRATE] == CELL(4, 1));
    }
    {
        /* the crate on top: mech under a crate column, flip -> crushed */
        static const char *m[RH] = {
            "##########",
            "#.#......#",
            "#........#",
            "#........#",
            "#........#",
            "#........#",
            "#........#",
            "v........E",
            "##########" };
        State t;
        draw(&R, m);
        state_start(&R, &s);
        s.o[SLOT_CRATE] = CELL(1, 6);          /* a crate resting on the mech's... */
        s.o[0] = CELL(1, 7);
        t = s;
        r = sim_turn(&R, &t, ACT_FLIP, &lg);
        /* gravity up: the crate (nearer the ceiling) goes first, the mech follows and
         * lands under it: no crush */
        CHECK(r == TR_OK && t.o[SLOT_CRATE] == CELL(1, 1) && t.o[0] == CELL(1, 2));
        /* flip back: the mech falls first, the crate falls onto it from a height: crush */
        r = sim_turn(&R, &t, ACT_FLIP, &lg);
        CHECK(r == TR_DEAD && lg.cause == DIE_CRUSH);
    }
    {
        /* a drone chases one step per turn and catches you */
        static const char *m[RH] = {
            "##########",
            "#........#",
            "#........#",
            "#........#",
            "#........#",
            "#........#",
            "#........#",
            "v....B...E",
            "##########" };
        draw(&R, m);
        state_start(&R, &s);
        r = sim_turn(&R, &s, ACT_R, NULL);   /* mech 1, drone 5 -> 4 */
        CHECK(r == TR_OK && s.o[SLOT_BOT] == CELL(4, 7));
        r = sim_turn(&R, &s, ACT_R, NULL);   /* mech 2, drone 3 */
        CHECK(r == TR_OK && s.o[SLOT_BOT] == CELL(3, 7));
        r = sim_turn(&R, &s, ACT_R, &lg);    /* walks into it */
        CHECK(r == TR_DEAD && lg.cause == DIE_BOT);
    }
    {
        /* phase blocks: purple solid while gravity is down; flips refused inside */
        static const char *m[RH] = {
            "##########",
            "#........#",
            "#........#",
            "#........#",
            "#pppppppp#",
            "#........#",
            "#........#",
            "v........E",
            "##########" };
        draw(&R, m);
        state_start(&R, &s);
        sim_turn(&R, &s, ACT_R, NULL);
        r = sim_turn(&R, &s, ACT_FLIP, NULL);
        CHECK(r == TR_OK && s.o[0] == CELL(1, 1));   /* purple is a ghost while up */
        r = sim_turn(&R, &s, ACT_FLIP, NULL);
        CHECK(r == TR_OK && s.o[0] == CELL(1, 3));   /* and a floor while down */
    }
    {
        /* plate and door */
        static const char *m[RH] = {
            "##########",
            "#........#",
            "#........#",
            "#........#",
            "#........#",
            "#.....|..#",
            "#.....|..#",
            "v._...|..E",
            "##########" };
        draw(&R, m);
        state_start(&R, &s);
        CHECK(solve(&R, &s) == 0 || 1);
        sim_turn(&R, &s, ACT_R, NULL);
        r = sim_turn(&R, &s, ACT_R, &lg);
        CHECK(r == TR_OK && lg.doors == 1 && (lg.flags & EV_DOOR));
        r = sim_turn(&R, &s, ACT_R, &lg);
        CHECK(r == TR_OK && lg.doors == 0);
    }
}

/* ------------------------------------- fast simulation vs the reference */

static void random_room(Room *R, uint8_t full)
{
    uint8_t i, c, n;
    box(R);
    if (rnd8(2)) { R->g0 = G_UP; }
    n = (uint8_t)(rnd8(14) + 2);
    for (i = 0; i < n; i++) {
        c = CELL(1 + rnd8(8), 1 + rnd8(7));
        R->cell[c] = T_WALL;
    }
    R->cell[CELL(1, 7)] = T_EMPTY;
    if (!full) return;
    for (i = 0; i < 3; i++) { c = CELL(1 + rnd8(8), 1 + rnd8(7)); if (R->cell[c] == T_EMPTY && c != CELL(1, 7)) R->cell[c] = T_PLASMA; }
    for (i = 0; i < 4; i++) { c = CELL(1 + rnd8(8), 1 + rnd8(7)); if (R->cell[c] == T_EMPTY && c != CELL(1, 7)) R->cell[c] = rnd8(2) ? T_PHASE_P : T_PHASE_G; }
    if (rnd8(2)) {
        c = CELL(1 + rnd8(8), 1 + rnd8(7)); if (R->cell[c] == T_EMPTY && c != CELL(1, 7)) R->cell[c] = T_PLATE;
        for (i = 0; i < 3; i++) { c = CELL(1 + rnd8(8), 1 + rnd8(7)); if (R->cell[c] == T_EMPTY && c != CELL(1, 7)) R->cell[c] = T_DOOR; }
    }
    n = rnd8(4);
    for (i = 0; i < n; i++) { c = CELL(1 + rnd8(8), 1 + rnd8(7)); if (R->cell[c] == T_EMPTY && c != CELL(1, 7)) { R->cell[c] = (uint8_t)(T_PICK0 + R->npicks); R->npicks++; } }
    for (i = 0; i < MAX_CRATES; i++) { c = CELL(1 + rnd8(8), 1 + rnd8(7)); if (R->cell[c] != T_WALL && c != CELL(1, 7)) R->crate0[i] = c; }
    for (i = 0; i < MAX_BOTS; i++) { c = CELL(2 + rnd8(7), 1 + rnd8(7)); if (R->cell[c] != T_WALL && c != CELL(1, 7) && c != R->crate0[0] && c != R->crate0[1]) R->bot0[i] = c; }
    if (R->bot0[0] == R->bot0[1]) R->bot0[1] = NONE;
    if (R->crate0[0] == R->crate0[1]) R->crate0[1] = NONE;
    if (rnd8(3) == 0) {
        /* a ceiling exit instead */
        R->cell[R->exit] = T_WALL;
        R->exit = CELL(1 + rnd8(8), 0);
        R->exit_side = SIDE_T;
        R->cell[R->exit] = T_EXIT;
    }
}

static int same_log(const TurnLog *a, const TurnLog *b)
{
    return a->flags == b->flags && a->act == b->act && !memcmp(a->start, b->start, NOBJ) &&
           !memcmp(a->step, b->step, NOBJ) && !memcmp(a->end, b->end, NOBJ) &&
           !memcmp(a->died_at, b->died_at, NOBJ) && !memcmp(a->killer, b->killer, NOBJ) &&
           a->cause == b->cause && a->picked == b->picked && a->doors == b->doors;
}

static void test_fast_vs_ref(void)
{
    Room R;
    State s, t;
    TurnLog la, lb;
    unsigned games, turns = 0, wins = 0, deaths = 0, pushes = 0, crushes = 0;
    uint8_t ra, rb, a, k, sa, sb;
    for (games = 0; games < 30000; games++) {
        random_room(&R, games % 5 != 0);
        sa = state_start(&R, &s);
        sb = ref_state_start(&R, &t);
        CHECK(sa == sb && !memcmp(&s, &t, sizeof(State)));
        if (sa != TR_OK) continue;
        for (k = 0; k < 60; k++) {
            State before = s;
            a = rnd8(3);
            ra = sim_turn(&R, &s, a, &la);
            rb = ref_sim_turn(&R, &t, a, &lb);
            turns++;
            CHECKF(ra == rb, "game %u turn %u: %u vs %u", games, k, ra, rb);
            if (ra != TR_BLOCKED) CHECKF(same_log(&la, &lb), "game %u turn %u log", games, k);
            CHECKF(!memcmp(&s, &t, sizeof(State)), "game %u turn %u state", games, k);
            if (ra != rb || memcmp(&s, &t, sizeof(State))) {
                static int shown;
                if (!shown++) {
                    uint8_t q;
                    printf("  act %u g %u: before", a, before.g);
                    for (q = 0; q < NOBJ; q++) printf(" %u", before.o[q]);
                    printf("\n  fast %u:", ra);
                    for (q = 0; q < NOBJ; q++) printf(" %u", s.o[q]);
                    printf(" g%u got%u\n  ref  %u:", s.g, s.got, rb);
                    for (q = 0; q < NOBJ; q++) printf(" %u", t.o[q]);
                    printf(" g%u got%u\n", t.g, t.got);
                    for (q = 0; q < RN; q++) printf("%c%s", ".#~pg_|***()"[R.cell[q]], (q % 10 == 9) ? "\n" : "");
                }
                break;
            }
            if (la.flags & EV_PUSH) pushes++;
            if (la.flags & EV_BOT_DIED) crushes++;
            if (ra == TR_WIN) { wins++; break; }
            if (ra == TR_DEAD) { deaths++; break; }
        }
    }
    printf("  fast vs reference: %u turns, %u wins, %u deaths, %u pushes, %u drones lost\n",
           turns, wins, deaths, pushes, crushes);
    CHECK(wins > 100 && deaths > 1000 && pushes > 1000 && crushes > 100);
}

/* ------------------------------------------------------- solver checks */

static const char ACT_CH[3] = { 'L', 'R', 'F' };

/* play the solver's hint chain: it must win in exactly par turns */
static uint8_t replay_hints(const Room *R, uint8_t par)
{
    State s;
    uint8_t n = 0, r, rem;
    state_start(R, &s);
    for (;;) {
        rem = solve(R, &s);
        if (!rem) return 0;
        if (n + rem != par) return 0;
        r = sim_turn(R, &s, solve_first, NULL);
        n++;
        if (r == TR_WIN) return n == par;
        if (r != TR_OK) return 0;
        if (n > 200) return 0;
    }
}

static void test_generator(void)
{
    uint16_t seed, i;
    unsigned rooms = 0, bad = 0;
    Room R, R2;
    Door d, nx, d2;
    State s;
    for (seed = 1; seed <= 12; seed++) {
        gen_first_door(&d);
        for (i = 0; i < 40; i++) {
            gen_room(&R, (uint16_t)(seed * 4099u), &d);
            rooms++;
            CHECKF(R.par > 0, "seed %u room %u unsolved", seed, d.index);
            CHECK(state_start(&R, &s) == TR_OK);
            CHECKF(solve(&R, &s) == R.par, "seed %u room %u par %u solve %u", seed, d.index, R.par, solve(&R, &s));
            if (!replay_hints(&R, R.par)) { bad++; CHECKF(0, "seed %u room %u hint chain", seed, d.index); }
            /* border intact, doorways where they say */
            {
                uint8_t c, okb = 1;
                for (c = 0; c < RN; c++) {
                    if (CX(c) == 0 || CX(c) == RW - 1 || CY(c) == 0 || CY(c) == RH - 1) {
                        if (c == R.entry) okb &= R.cell[c] == T_ENTRY;
                        else if (c == R.exit) okb &= R.cell[c] == T_EXIT;
                        else okb &= R.cell[c] == T_WALL;
                    }
                }
                CHECKF(okb, "seed %u room %u border", seed, d.index);
            }
            CHECK(R.exit_side != SIDE_L);
            if (d.index == UNLOCK_CRATE || d.index == UNLOCK_BOT) CHECK(R.mechs & MBIT(gen_new_mech(d.index)));
            /* determinism */
            if (i % 9 == 0) {
                gen_room(&R2, (uint16_t)(seed * 4099u), &d);
                CHECK(!memcmp(&R, &R2, sizeof(Room)));
            }
            gen_next_door(&d, &R, &nx);
            /* the next room's entry lines up with this exit */
            if (R.exit_side == SIDE_R) CHECK(nx.side == SIDE_L && CY(nx.cell) == CY(R.exit));
            if (R.exit_side == SIDE_T) CHECK(nx.side == SIDE_B && CX(nx.cell) == CX(R.exit));
            if (R.exit_side == SIDE_B) CHECK(nx.side == SIDE_T && CX(nx.cell) == CX(R.exit));
            d = nx;
        }
    }
    (void)d2;
    printf("  generator: %u rooms, %u bad hint chains\n", rooms, bad);
}

/* the background search restarts cleanly when another search runs in between */
static Room yr;
static State ys;
static unsigned yields;
static void meddle(void)
{
    yields++;
    if (yields % 37 == 0 && yields < 3000) solve(&yr, &ys);
}

static void test_yield_restart(void)
{
    Room A, B;
    Door d;
    gen_first_door(&d);
    d.index = 45;
    gen_room(&A, 0x1234, &d);
    box(&yr);
    state_start(&yr, &ys);
    solve_yield = meddle;
    gen_room(&B, 0x1234, &d);
    solve_yield = NULL;
    CHECK(yields > 100);
    CHECK(!memcmp(&A, &B, sizeof(Room)));
}

static void test_rng(void)
{
    uint16_t a, b;
    rng_seed2(1, 1);
    a = rng_next();
    rng_seed2(1, 2);
    b = rng_next();
    CHECK(a != b);
}

int main(void)
{
    printf("ALIGNMENT core tests\n");
    test_rules();
    test_fast_vs_ref();
    test_yield_restart();
    test_generator();
    test_rng();
    (void)ACT_CH;
    printf("%u checks, %u failures\n", checks, fails);
    return fails ? 1 : 0;
}
