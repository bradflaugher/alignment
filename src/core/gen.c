/* ALIGNMENT - the room generator.
 *
 * For (seed, room) it rolls a few random rooms from the unlocked mechanics, then
 * hill-climbs the closest one. Each candidate is judged by a full exploration of its
 * state space with the exit sealed: that gives, for every border cell, the fewest
 * turns out through a doorway there (with every energy cell), so the exit goes to the
 * cell that best fits the room's difficulty window (par and number of flips). A room
 * that introduces or focuses on a mechanic must need it: with the mechanic stripped
 * out, par has to change. A last pass removes hazards and props that par doesn't need.
 * The work is counted in states visited, so the result is identical on every machine;
 * a hand-made fallback guarantees a way out. */
#ifdef __SDCC
#pragma bank 255
#endif
#include <string.h>
#include "gen.h"
#include "solver.h"
#include "rng.h"

uint16_t gen_cost, gen_searches;
uint8_t gen_score;

static Room cand, best, tmp;
static GenParams P;
static uint8_t best_score;
static uint8_t want_side;
static uint16_t budget;

#define SCORE_NONE 255

static uint8_t rnd(uint8_t n)
{
    return rng_range(n);
}

static void charge(void)
{
    gen_cost += (uint16_t)(solve_visited + GEN_SEARCH_OVERHEAD);
    gen_searches++;
}

/* ------------------------------------------------------------------ doors */

void gen_first_door(Door *d) CORE_BANKED
{
    d->index = 1;
    d->side = SIDE_L;
    d->cell = CELL(0, RH - 2);
    d->rx = 0;
    d->ry = 0;
}

void gen_next_door(const Door *d, const Room *R, Door *nx) CORE_BANKED
{
    uint8_t e = R->exit;
    nx->index = (uint16_t)(d->index + 1);
    nx->rx = d->rx;
    nx->ry = d->ry;
    if (R->exit_side == SIDE_R) {
        nx->side = SIDE_L;
        nx->cell = CELL(0, CY(e));
        nx->rx++;
    } else if (R->exit_side == SIDE_T) {
        nx->side = SIDE_B;
        nx->cell = CELL(CX(e), RH - 1);
        nx->ry--;
    } else {
        nx->side = SIDE_T;
        nx->cell = CELL(CX(e), 0);
        nx->ry++;
    }
}

/* ------------------------------------------------------------- parameters */

static const uint8_t unlock_at[NUM_MECH] = {
    UNLOCK_PLASMA, UNLOCK_PICK, UNLOCK_CRATE, UNLOCK_BOT, UNLOCK_PHASE, UNLOCK_DOOR
};

static uint8_t unlocked(uint16_t n)
{
    uint8_t m, u = 0;
    for (m = 0; m < NUM_MECH; m++)
        if (n >= unlock_at[m])
            u |= MBIT(m);
    return u;
}

uint8_t gen_new_mech(uint16_t n) CORE_BANKED
{
    uint8_t m;
    for (m = 0; m < NUM_MECH; m++)
        if (n == unlock_at[m])
            return m;
    return 0xFF;
}

static uint8_t newest(uint16_t n)
{
    uint8_t m, r = 0xFF;
    for (m = 0; m < NUM_MECH; m++)
        if (n >= unlock_at[m])
            r = m;
    return r;
}

static uint8_t count_bits(uint8_t v)
{
    uint8_t c = 0;
    while (v) { c += v & 1; v >>= 1; }
    return c;
}

/* uses the rng: call after seeding */
void gen_params(uint16_t n, GenParams *p) CORE_BANKED
{
    uint8_t un = unlocked(n), m, k, tries;
    uint8_t lvl = n > 200 ? 200 : (uint8_t)n;
    uint8_t deck_pos = (uint8_t)((n - 1) % DECK_ROOMS);   /* 0..7 within the deck */

    memset(p, 0, sizeof(GenParams));
    p->featured = gen_new_mech(n);
    p->focus = 0xFF;
    if (p->featured != 0xFF) {
        p->mechs = MBIT(p->featured);
        p->focus = p->featured;
    } else if (n < FREEFORM_ROOM) {
        m = newest(n);
        if (m != 0xFF) {
            p->mechs = MBIT(m);
            p->focus = m;
            /* now and then an older mechanic joins in */
            if (m > 0 && rnd(2)) p->mechs |= MBIT(rnd(m));
        }
    } else {
        k = (uint8_t)(2 + rnd(n >= 60 ? 3 : 2));          /* 2-3, later 2-4 */
        for (tries = 0; tries < 24 && count_bits(p->mechs) < k; tries++) {
            m = rnd(NUM_MECH);
            if (un & MBIT(m)) p->mechs |= MBIT(m);
        }
        do { m = rnd(NUM_MECH); } while (!(p->mechs & MBIT(m)));
        p->focus = m;
    }

    /* difficulty: par is counted in turns (every step counts), flips are the real
     * decisions. The first room of a deck is a breather, the last one a test. */
    p->flips_min = (uint8_t)(1 + lvl / 14);
    if (p->flips_min > 5) p->flips_min = 5;
    p->par_min = (uint8_t)(7 + lvl / 6);
    if (p->par_min > 18) p->par_min = 18;
    if (deck_pos == 0 && n > 1) {
        p->par_min = (uint8_t)(p->par_min * 3 / 4);
        if (p->flips_min > 1) p->flips_min--;
    } else if (deck_pos == DECK_ROOMS - 1) {
        p->par_min = (uint8_t)(p->par_min + 2);
        p->flips_min++;
    }
    if (p->featured != 0xFF) {
        /* a lesson: short and clear */
        p->par_min = (uint8_t)(6 + lvl / 8);
        p->flips_min = 1;
    }
    p->par_max = (uint8_t)(p->par_min + 10);

    p->walls = (uint8_t)(3 + rnd(3));
    if (p->mechs & MBIT(M_PICK))
        p->picks = p->featured == M_PICK ? 1 : (uint8_t)(1 + (n >= 14) + (n >= 40 && rnd(2)));
    if (p->mechs & MBIT(M_CRATE))
        p->crates = p->featured == M_CRATE ? 1 : (uint8_t)(1 + (n >= 30 && rnd(2)));
    if (p->mechs & MBIT(M_BOT))
        p->bots = p->featured == M_BOT ? 1 : (uint8_t)(1 + (n >= 36 && rnd(2)));
}

/* ----------------------------------------------------------------- layout */

static uint8_t interior(uint8_t c)
{
    uint8_t x = CX(c), y = CY(c);
    return (uint8_t)(x > 0 && x < RW - 1 && y > 0 && y < RH - 1);
}

/* cells kept clear: the one inside the entry and the one inside the exit */
static uint8_t inner_of(uint8_t door, uint8_t side)
{
    if (side == SIDE_L) return (uint8_t)(door + 1);
    if (side == SIDE_R) return (uint8_t)(door - 1);
    if (side == SIDE_T) return (uint8_t)(door + RW);
    return (uint8_t)(door - RW);
}

static uint8_t is_object(const Room *R, uint8_t c)
{
    uint8_t i;
    for (i = 0; i < MAX_CRATES; i++) if (R->crate0[i] == c) return 1;
    for (i = 0; i < MAX_BOTS; i++) if (R->bot0[i] == c) return 1;
    return 0;
}

static uint8_t protected_cell(const Room *R, uint8_t c)
{
    if (c == inner_of(R->entry, R->entry_side)) return 1;
    if (R->exit && c == inner_of(R->exit, R->exit_side)) return 1;
    /* the column a ceiling/floor entry drops through, two cells deep */
    if (R->entry_side != SIDE_L && CX(c) == CX(R->entry)) {
        uint8_t y = CY(c);
        if (R->entry_side == SIDE_T ? y <= 2 : y >= RH - 3) return 1;
    }
    return 0;
}

static uint8_t rand_interior(void)
{
    return CELL(1 + rnd(RW - 2), 1 + rnd(RH - 2));
}

/* an empty interior cell, not protected, not holding an object; NONE if none found */
static uint8_t rand_empty(const Room *R)
{
    uint8_t tries, c;
    for (tries = 0; tries < 40; tries++) {
        c = rand_interior();
        if (R->cell[c] == T_EMPTY && !protected_cell(R, c) && !is_object(R, c))
            return c;
    }
    return NONE;
}

/* an empty cell on a surface (a wall right above or below it) */
static uint8_t rand_surface(const Room *R)
{
    uint8_t tries, c;
    for (tries = 0; tries < 40; tries++) {
        c = rand_empty(R);
        if (c == NONE) return NONE;
        if (R->cell[c + RW] == T_WALL || R->cell[c - RW] == T_WALL)
            return c;
    }
    return NONE;
}

static void set_wall(Room *R, uint8_t x, uint8_t y)
{
    uint8_t c;
    if (x < 1 || x > RW - 2 || y < 1 || y > RH - 2) return;
    c = CELL(x, y);
    if (protected_cell(R, c)) return;
    R->cell[c] = T_WALL;
}

static void structure(Room *R, uint8_t pieces)
{
    uint8_t i, k, x, y, len, j;
    for (i = 0; i < pieces; i++) {
        k = rnd(12);
        if (k < 7) {
            /* a ledge, often grown out of a side wall */
            y = (uint8_t)(2 + rnd(RH - 4));
            len = (uint8_t)(2 + rnd(4));
            j = rnd(3);
            x = j == 0 ? 1 : j == 1 ? (uint8_t)(RW - 1 - len) : (uint8_t)(2 + rnd(RW - 3 - len));
            for (j = 0; j < len; j++) set_wall(R, (uint8_t)(x + j), y);
        } else if (k < 9) {
            /* a pillar hanging from the ceiling */
            x = (uint8_t)(2 + rnd(RW - 4));
            len = (uint8_t)(1 + rnd(4));
            for (j = 0; j < len; j++) set_wall(R, x, (uint8_t)(1 + j));
        } else if (k < 11) {
            /* a pillar rising from the floor */
            x = (uint8_t)(2 + rnd(RW - 4));
            len = (uint8_t)(1 + rnd(4));
            for (j = 0; j < len; j++) set_wall(R, x, (uint8_t)(RH - 2 - j));
        } else {
            /* a block in mid-air */
            x = (uint8_t)(2 + rnd(RW - 5));
            y = (uint8_t)(2 + rnd(RH - 5));
            set_wall(R, x, y);
            set_wall(R, (uint8_t)(x + 1), y);
            set_wall(R, x, (uint8_t)(y + 1));
            set_wall(R, (uint8_t)(x + 1), (uint8_t)(y + 1));
        }
    }
}

static void place_plasma(Room *R)
{
    uint8_t n = (uint8_t)(1 + rnd(3)), i, c;
    for (i = 0; i < n; i++) {
        c = rand_surface(R);
        if (c == NONE) continue;
        R->cell[c] = T_PLASMA;
        /* sometimes a strip of two */
        if (rnd(2) && interior((uint8_t)(c + 1)) && R->cell[c + 1] == T_EMPTY &&
            !protected_cell(R, (uint8_t)(c + 1)) && !is_object(R, (uint8_t)(c + 1)))
            R->cell[c + 1] = T_PLASMA;
    }
}

static void place_phase(Room *R)
{
    uint8_t n = (uint8_t)(2 + rnd(4)), i, c, t, run, j;
    for (i = 0; i < n; i++) {
        t = rnd(2) ? T_PHASE_P : T_PHASE_G;
        c = rand_interior();
        if (protected_cell(R, c) || is_object(R, c)) continue;
        if (R->cell[c] != T_EMPTY && R->cell[c] != T_WALL) continue;
        /* a short horizontal run of one colour */
        run = (uint8_t)(1 + rnd(3));
        for (j = 0; j < run && CX(c) + j < RW - 1; j++) {
            uint8_t d = (uint8_t)(c + j);
            if (protected_cell(R, d) || is_object(R, d)) break;
            if (R->cell[d] != T_EMPTY && R->cell[d] != T_WALL) break;
            R->cell[d] = t;
        }
    }
}

static void place_door(Room *R)
{
    uint8_t c, x, y, h, j, d;
    c = rand_surface(R);
    if (c == NONE) return;
    R->cell[c] = T_PLATE;
    /* a vertical barrier of 1-3 door cells, from a wall */
    x = (uint8_t)(2 + rnd(RW - 4));
    h = (uint8_t)(1 + rnd(3));
    y = rnd(2) ? 1 : (uint8_t)(RH - 1 - h);
    for (j = 0; j < h; j++) {
        d = CELL(x, (uint8_t)(y + j));
        if (R->cell[d] == T_EMPTY && !protected_cell(R, d) && !is_object(R, d))
            R->cell[d] = T_DOOR;
    }
}

static uint8_t place_objects(Room *R)
{
    uint8_t i, c, tries, ex = CX(R->entry);
    State s;
    for (i = 0; i < P.crates; i++) {
        c = rand_empty(R);
        if (c != NONE) R->crate0[i] = c;
    }
    for (i = 0; i < P.bots; i++) {
        for (tries = 0; tries < 12; tries++) {
            c = rand_empty(R);
            if (c == NONE) break;
            if ((CX(c) > ex ? CX(c) - ex : ex - CX(c)) >= 3 && R->cell[c] != T_PLASMA) {
                R->bot0[i] = c;
                break;
            }
        }
    }
    /* let them fall into place */
    if (state_start(R, &s) == TR_DEAD)
        return 0;
    for (i = 0; i < MAX_CRATES; i++) R->crate0[i] = s.o[SLOT_CRATE + i];
    for (i = 0; i < MAX_BOTS; i++) R->bot0[i] = s.o[SLOT_BOT + i];
    return 1;
}

static void mechs_present(Room *R)
{
    uint8_t i, t, m = 0;
    for (i = 0; i < RN; i++) {
        t = R->cell[i];
        if (t == T_PLASMA) m |= MBIT(M_PLASMA);
        else if (t == T_PHASE_P || t == T_PHASE_G) m |= MBIT(M_PHASE);
        else if (t == T_DOOR || t == T_PLATE) m |= MBIT(M_DOOR);
    }
    if (R->npicks) m |= MBIT(M_PICK);
    for (i = 0; i < MAX_CRATES; i++) if (R->crate0[i]) m |= MBIT(M_CRATE);
    for (i = 0; i < MAX_BOTS; i++) if (R->bot0[i]) m |= MBIT(M_BOT);
    R->mechs = m;
}

/* ------------------------------------------------------------- evaluation */

static uint8_t score_of(uint8_t par, uint8_t flips)
{
    uint8_t s = 0;
    if (!par) return SCORE_NONE;
    if (par < P.par_min) s = (uint8_t)(P.par_min - par);
    else if (par > P.par_max) s = (uint8_t)(par - P.par_max);
    if (flips < P.flips_min) s = (uint8_t)(s + 2 * (P.flips_min - flips));
    return s;
}

static uint8_t side_of(uint8_t c)
{
    uint8_t x = CX(c), y = CY(c);
    if (x == RW - 1) return SIDE_R;
    if (x == 0) return SIDE_L;
    return y == 0 ? SIDE_T : SIDE_B;
}

static uint8_t side_allowed(uint8_t entry_side, uint8_t s)
{
    if (s == SIDE_L || s == entry_side) return 0;
    if (entry_side == SIDE_B && s == SIDE_B) return 0;
    if (entry_side == SIDE_T && s == SIDE_T) return 0;
    return 1;
}

static uint8_t strip(Room *R, uint8_t m)
{
    uint8_t i, t;
    for (i = 0; i < RN; i++) {
        t = R->cell[i];
        if ((m == M_PLASMA && t == T_PLASMA) ||
            (m == M_PHASE && (t == T_PHASE_P || t == T_PHASE_G)) ||
            (m == M_DOOR && (t == T_DOOR || t == T_PLATE)) ||
            (m == M_PICK && IS_PICK(t)))
            R->cell[i] = T_EMPTY;
    }
    if (m == M_PICK) R->npicks = 0;
    if (m == M_CRATE) memset(R->crate0, 0, sizeof(R->crate0));
    if (m == M_BOT) memset(R->bot0, 0, sizeof(R->bot0));
    return 1;
}

static uint8_t solve_room(const Room *R)
{
    State s;
    uint8_t par;
    if (state_start(R, &s) == TR_DEAD) return 0;
    par = solve_bg(R, &s);
    charge();
    return par;
}

/* Does the focus mechanic matter? Crates and plates must help (without them par gets
 * longer, or there is no way out); everything else must change par either way. */
static uint8_t focus_matters(const Room *R)
{
    uint8_t par, i;
    if (P.focus == 0xFF || !(R->mechs & MBIT(P.focus))) return P.focus == 0xFF;
    memcpy(&tmp, R, sizeof(Room));
    if (P.focus == M_DOOR) {
        /* the plates go, the doors stay shut */
        for (i = 0; i < RN; i++)
            if (tmp.cell[i] == T_PLATE) tmp.cell[i] = T_EMPTY;
    } else {
        strip(&tmp, P.focus);
    }
    par = solve_room(&tmp);
    if (P.focus == M_CRATE || P.focus == M_DOOR)
        return (uint8_t)(!par || par > R->par);
    return (uint8_t)(par != R->par);
}

/* Explore the room with its exit sealed and put the exit where it fits best.
 * Fills R->exit / exit_side / par / flips; returns the score. */
static uint8_t place_exit(Room *R)
{
    State s;
    uint8_t c, sc, bsc = SCORE_NONE, bc = NONE, bd = 0, d, side, pref;
    if (R->exit) {
        R->cell[R->exit] = T_WALL;
        R->exit = NONE;
    }
    R->par = 0;
    if (state_start(R, &s) == TR_DEAD) return SCORE_NONE;
    solve_limit = (uint8_t)(P.par_max + 4);   /* deeper exits score worse anyway */
    explore_bg(R, &s);
    solve_limit = SOLVE_MAX_DEPTH;
    charge();
    for (c = 0; c < RN; c++) {
        d = explore_depth[c];
        if (!d) continue;
        side = side_of(c);
        if (!side_allowed(R->entry_side, side)) continue;
        if (c == R->entry) continue;
        sc = score_of(d, explore_flips[c]);
        pref = side == want_side ? 0 : 1;   /* the side the route wants, if it can */
        sc = (uint8_t)(sc + pref);
        if (sc < bsc || (sc == bsc && d > bd)) {
            bsc = sc;
            bc = c;
            bd = d;
        }
    }
    if (bc == NONE) return SCORE_NONE;
    R->exit = bc;
    R->exit_side = side_of(bc);
    R->cell[bc] = T_EXIT;
    R->par = bd;
    R->flips = explore_flips[bc];
    /* The doorway is a cell the mech can step or drop into before the exit opens,
     * which the sealed exploration didn't see: solve the room for real. */
    R->par = solve_room(R);
    R->flips = solve_flips;
    return score_of(R->par, R->flips);
}

static void place_picks(Room *R)
{
    State s;
    uint8_t i, c, tries;
    for (c = 0; c < RN; c++)
        if (IS_PICK(R->cell[c])) R->cell[c] = T_EMPTY;
    R->npicks = 0;
    if (!P.picks) return;
    if (R->exit) {
        R->cell[R->exit] = T_WALL;
        R->exit = NONE;
    }
    if (state_start(R, &s) == TR_DEAD) return;
    explore_bg(R, &s);
    charge();
    for (i = 0; i < P.picks; i++) {
        for (tries = 0; tries < 24; tries++) {
            c = rand_interior();
            if (R->cell[c] != T_EMPTY || is_object(R, c) || c == s.o[0]) continue;
            if (!(explore_reach[c >> 3] & (1u << (c & 7)))) continue;
            R->cell[c] = (uint8_t)(T_PICK0 + i);
            R->npicks++;
            break;
        }
        if (R->npicks != (uint8_t)(i + 1)) break;
    }
}

/* full judgement of a candidate: exit placement, then the focus rule */
static uint8_t evaluate(Room *R)
{
    uint8_t sc;
    mechs_present(R);
    sc = place_exit(R);
    if (sc == SCORE_NONE) return sc;
    /* the focus rule costs a search: only ask once the room is in its window */
    if (sc || !focus_matters(R)) sc = (uint8_t)(sc + 6);
    return sc;
}

static void roll(Room *R, const Door *d)
{
    uint8_t i;
    room_clear(R);
    R->index = d->index;
    R->featured = P.featured;
    for (i = 0; i < RN; i++)
        if (!interior(i)) R->cell[i] = T_WALL;
    R->entry = d->cell;
    R->entry_side = d->side;
    R->cell[d->cell] = T_ENTRY;
    if (d->side == SIDE_T) R->g0 = G_DOWN;
    else if (d->side == SIDE_B) R->g0 = G_UP;
    else R->g0 = d->index <= 2 ? G_DOWN : rnd(2);
    structure(R, P.walls);
    if (P.mechs & MBIT(M_PLASMA)) place_plasma(R);
    if (P.mechs & MBIT(M_PHASE)) place_phase(R);
    if (P.mechs & MBIT(M_DOOR)) place_door(R);
    if (!place_objects(R)) {
        memset(R->crate0, 0, sizeof(R->crate0));
        memset(R->bot0, 0, sizeof(R->bot0));
    }
    place_picks(R);
}

/* ------------------------------------------------------------------- edits */

static uint8_t mutate(Room *R)
{
    uint8_t k = rnd(10), c, d, i, t;
    State s;
    if (k < 4) {
        /* grow or shrink the structure by one cell (never a lone floating block) */
        c = rand_interior();
        if (protected_cell(R, c) || is_object(R, c)) return 0;
        t = R->cell[c];
        if (t == T_WALL) {
            R->cell[c] = T_EMPTY;
        } else if (t == T_EMPTY) {
            if (R->cell[c - 1] != T_WALL && R->cell[c + 1] != T_WALL &&
                R->cell[c - RW] != T_WALL && R->cell[c + RW] != T_WALL)
                return 0;
            R->cell[c] = T_WALL;
        } else {
            return 0;
        }
    } else if (k < 6) {
        /* move a feature (hazard, phase block, door, plate or energy cell) */
        c = rand_interior();
        t = R->cell[c];
        if (t == T_EMPTY || t == T_WALL) return 0;
        d = rand_empty(R);
        if (d == NONE) return 0;
        R->cell[d] = t;
        R->cell[c] = T_EMPTY;
    } else if (k < 8) {
        /* move a crate or drone */
        i = rnd(MAX_CRATES + MAX_BOTS);
        d = rand_empty(R);
        if (d == NONE) return 0;
        if (i < MAX_CRATES) {
            if (!R->crate0[i]) return 0;
            R->crate0[i] = d;
        } else {
            if (!R->bot0[i - MAX_CRATES]) return 0;
            if ((CX(d) > CX(R->entry) ? CX(d) - CX(R->entry) : CX(R->entry) - CX(d)) < 3) return 0;
            R->bot0[i - MAX_CRATES] = d;
        }
        if (state_start(R, &s) == TR_DEAD) return 0;
        for (i = 0; i < MAX_CRATES; i++) R->crate0[i] = s.o[SLOT_CRATE + i];
        for (i = 0; i < MAX_BOTS; i++) R->bot0[i] = s.o[SLOT_BOT + i];
        if (P.bots && !R->bot0[0] && !R->bot0[1]) return 0;
    } else if (k < 9) {
        /* swap a phase block's colour */
        c = rand_interior();
        t = R->cell[c];
        if (t == T_PHASE_P) R->cell[c] = T_PHASE_G;
        else if (t == T_PHASE_G) R->cell[c] = T_PHASE_P;
        else return 0;
    } else {
        /* the airlock's gravity */
        if (R->entry_side != SIDE_L) return 0;
        R->g0 ^= 1;
    }
    return 1;
}

/* ----------------------------------------------------------------- tidy up */

static void tidy(Room *R)
{
    uint8_t c, t, par, i, saved;
    for (c = 0; c < RN && gen_cost < GEN_TIDY_BUDGET; c++) {
        t = R->cell[c];
        if (t != T_PLASMA && t != T_PHASE_P && t != T_PHASE_G && t != T_DOOR) continue;
        R->cell[c] = T_EMPTY;
        par = solve_room(R);
        if (par == R->par) {
            /* (the focus rule is re-checked once at the end) */
            continue;
        }
        R->cell[c] = t;
    }
    for (i = 0; i < MAX_CRATES + MAX_BOTS && gen_cost < GEN_TIDY_BUDGET; i++) {
        uint8_t *slot = i < MAX_CRATES ? &R->crate0[i] : &R->bot0[i - MAX_CRATES];
        if (!*slot) continue;
        saved = *slot;
        *slot = NONE;
        par = solve_room(R);
        if (par != R->par) *slot = saved;
    }
    /* a plate without doors, or doors without a plate, do nothing: clear the set */
    {
        uint8_t plates = 0, doors = 0;
        for (c = 0; c < RN; c++) {
            if (R->cell[c] == T_PLATE) plates++;
            if (R->cell[c] == T_DOOR) doors++;
        }
        if (!plates || !doors)
            for (c = 0; c < RN; c++)
                if (R->cell[c] == T_PLATE || R->cell[c] == T_DOOR) R->cell[c] = T_EMPTY;
    }
    mechs_present(R);
}

/* ---------------------------------------------------------------- fallback */

static void fallback(Room *R, const Door *d)
{
    uint8_t i;
    room_clear(R);
    R->index = d->index;
    R->fallback = 1;
    for (i = 0; i < RN; i++)
        if (!interior(i)) R->cell[i] = T_WALL;
    R->entry = d->cell;
    R->entry_side = d->side;
    R->cell[d->cell] = T_ENTRY;
    /* an empty hall: walk to the right wall on whichever side gravity holds you */
    R->g0 = d->side == SIDE_B ? G_UP : G_DOWN;
    R->exit = R->g0 == G_UP ? CELL(RW - 1, 1) : CELL(RW - 1, RH - 2);
    R->exit_side = SIDE_R;
    R->cell[R->exit] = T_EXIT;
    R->par = solve_room(R);
    R->flips = solve_flips;
}

/* ------------------------------------------------------------------- main */

void gen_room(Room *R, uint16_t seed, const Door *d) CORE_BANKED
{
    uint8_t attempt, stall, sc, rolls, teach;
    uint16_t steps;

    rng_seed2(seed, d->index);
    gen_params(d->index, &P);
    gen_cost = 0;
    gen_searches = 0;
    teach = (uint8_t)(P.featured != 0xFF);
    budget = teach ? (uint16_t)(GEN_BUDGET + GEN_BUDGET / 2) : GEN_BUDGET;
    /* where the route would like to go next */
    if (d->side == SIDE_L) want_side = rnd(4) < 2 ? SIDE_R : (rnd(2) ? SIDE_T : SIDE_B);
    else want_side = rnd(2) ? SIDE_R : d->side == SIDE_B ? SIDE_T : SIDE_B;

    best_score = SCORE_NONE;
    rolls = teach ? 10 : 6;
    for (attempt = 0; attempt < rolls && gen_cost < budget; attempt++) {
        roll(&cand, d);
        sc = evaluate(&cand);
        if (sc < best_score) {
            best_score = sc;
            memcpy(&best, &cand, sizeof(Room));
        }
        if (!best_score) break;
    }

    /* hill-climb: small edits that don't take the room further from its window */
    stall = 0;
    for (steps = 0; best_score && best_score != SCORE_NONE && gen_cost < budget && steps < 400; steps++) {
        memcpy(&cand, &best, sizeof(Room));
        if (!mutate(&cand)) continue;
        if (P.picks && (steps & 7) == 7) place_picks(&cand);
        sc = evaluate(&cand);
        if (sc <= best_score) {
            if (sc < best_score) stall = 0;
            best_score = sc;
            memcpy(&best, &cand, sizeof(Room));
        } else if (++stall >= 24) {
            /* stuck: start from a fresh roll */
            roll(&cand, d);
            sc = evaluate(&cand);
            if (sc < best_score) {
                best_score = sc;
                memcpy(&best, &cand, sizeof(Room));
            }
            stall = 0;
        }
    }
    /* nothing solvable within the budget: keep rolling a little more, then give up */
    while (best_score == SCORE_NONE && gen_cost < budget + budget / 2) {
        roll(&cand, d);
        sc = evaluate(&cand);
        if (sc < best_score) {
            best_score = sc;
            memcpy(&best, &cand, sizeof(Room));
        }
    }
    if (best_score == SCORE_NONE) {
        fallback(R, d);
        return;
    }
    gen_score = best_score;
    memcpy(R, &best, sizeof(Room));
    tidy(R);
    R->featured = P.featured;
}
