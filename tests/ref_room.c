/* ALIGNMENT - the rules. One turn:
 *   1. the action: step left / right (pushing a crate if one is in the way), or flip
 *      gravity (refused while anything sits inside a phase block that would turn solid);
 *   2. every drone takes one step towards the mech's column (the nearest one first);
 *   3. everything loose falls with gravity, the object nearest the "ground" first.
 *      A crate that falls onto a drone crushes it; a crate or drone that falls onto the
 *      mech crushes it; the mech falling onto a drone is zapped; anything alive that
 *      enters plasma is destroyed. Doors re-check their plates after each fall pass.
 * The mech leaves through the exit (walking out of a side doorway, or falling out of a
 * ceiling / floor one) once every energy cell is collected.
 *
 * This file is in bank 0 on the Game Boy: the game and the solver both call ref_sim_core. */
#include <string.h>
#include "room.h"
#include "ref_room.h"

const uint8_t ref_cell_x[RN] = {
    0,1,2,3,4,5,6,7,8,9, 0,1,2,3,4,5,6,7,8,9, 0,1,2,3,4,5,6,7,8,9,
    0,1,2,3,4,5,6,7,8,9, 0,1,2,3,4,5,6,7,8,9, 0,1,2,3,4,5,6,7,8,9,
    0,1,2,3,4,5,6,7,8,9, 0,1,2,3,4,5,6,7,8,9, 0,1,2,3,4,5,6,7,8,9,
};
const uint8_t ref_cell_y[RN] = {
    0,0,0,0,0,0,0,0,0,0, 1,1,1,1,1,1,1,1,1,1, 2,2,2,2,2,2,2,2,2,2,
    3,3,3,3,3,3,3,3,3,3, 4,4,4,4,4,4,4,4,4,4, 5,5,5,5,5,5,5,5,5,5,
    6,6,6,6,6,6,6,6,6,6, 7,7,7,7,7,7,7,7,7,7, 8,8,8,8,8,8,8,8,8,8,
};

uint8_t r_o[NOBJ];
uint8_t r_g, r_got;

static uint8_t r_cell[RN];
static uint8_t r_occ[RN];       /* slot + 1 of the object in a cell, 0 = free */
static uint8_t r_full, r_exit, r_exit_side, r_has_plate, r_doors;

void ref_room_clear(Room *R)
{
    memset(R, 0, sizeof(Room));
    R->featured = 0xFF;
}

uint8_t ref_room_full(const Room *R)
{
    return (uint8_t)((1u << R->npicks) - 1u);
}

void ref_sim_load(const Room *R)
{
    uint8_t i;
    memcpy(r_cell, R->cell, RN);
    memset(r_occ, 0, RN);
    r_full = ref_room_full(R);
    r_exit = R->exit;
    r_exit_side = R->exit_side;
    r_has_plate = 0;
    for (i = 0; i < RN; i++)
        if (r_cell[i] == T_PLATE)
            r_has_plate = 1;
}

/* kind: 0 mech, 1 crate, 2 drone */
static uint8_t solid_for(uint8_t t, uint8_t kind)
{
    switch (r_cell[t]) {
    case T_WALL:
        return 1;
    case T_PHASE_P:
        return (uint8_t)(r_g == G_DOWN);
    case T_PHASE_G:
        return (uint8_t)(r_g == G_UP);
    case T_DOOR:
        return (uint8_t)!r_doors;
    case T_ENTRY:
    case T_EXIT:
        return kind;            /* only the mech goes through doorways */
    }
    return 0;
}

static uint8_t calc_doors(void)
{
    uint8_t i, c;
    if (!r_has_plate)
        return 0;
    for (i = 0; i < NOBJ; i++) {
        c = r_o[i];
        if (c && r_cell[c] == T_PLATE)
            return 1;
    }
    return 0;
}

static uint8_t kind_of(uint8_t slot)
{
    if (slot == 0) return 0;
    return slot < SLOT_BOT ? 1 : 2;
}

static void pick_at(uint8_t c, TurnLog *lg)
{
    uint8_t t = r_cell[c];
    uint8_t bit;
    if (!IS_PICK(t))
        return;
    bit = (uint8_t)(1u << (t - T_PICK0));
    if (r_got & bit)
        return;
    r_got |= bit;
    if (lg) {
        lg->picked |= bit;
        lg->flags |= EV_PICK;
        if (r_got == r_full)
            lg->flags |= EV_EXIT_OPEN;
    }
}

static uint8_t res;

static void kill_bot(uint8_t slot, uint8_t at, uint8_t killer, TurnLog *lg)
{
    r_occ[r_o[slot]] = 0;
    r_o[slot] = NONE;
    if (lg) {
        lg->died_at[slot] = at;
        lg->killer[slot] = killer;
        lg->flags |= EV_BOT_DIED;
    }
}

static void mech_dies(uint8_t at, uint8_t cause, uint8_t killer, TurnLog *lg)
{
    res = TR_DEAD;
    if (lg) {
        lg->flags |= EV_DEAD;
        lg->cause = cause;
        lg->died_at[0] = at;
        lg->killer[0] = killer;
    }
}

/* one fall pass; returns 1 if anything moved */
static uint8_t fall_pass(TurnLog *lg)
{
    uint8_t order[NOBJ];
    uint8_t n = 0, i, j, s, c, nc, k, y, dist, moved = 0, kind;
    int8_t dy = r_g == G_DOWN ? RW : -RW;

    /* objects sorted so the one nearest the ground (along gravity) falls first */
    for (i = 0; i < NOBJ; i++) {
        if (!r_o[i]) continue;
        y = CY(r_o[i]);
        if (r_g == G_UP) y = (uint8_t)(RH - y);
        j = n;
        while (j && (r_g == G_DOWN ? CY(r_o[order[j - 1]]) : (uint8_t)(RH - CY(r_o[order[j - 1]]))) < y) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
        n++;
    }
    for (i = 0; i < n; i++) {
        s = order[i];
        c = r_o[s];
        if (!c) continue;               /* crushed earlier in this pass */
        kind = kind_of(s);
        dist = 0;
        for (;;) {
            y = CY(c);
            if ((r_g == G_DOWN && y == RH - 1) || (r_g == G_UP && y == 0)) {
                /* falling off the screen: only the mech, only out of an open exit */
                if (s == 0 && c == r_exit && r_got == r_full &&
                    r_exit_side == (r_g == G_DOWN ? SIDE_B : SIDE_T)) {
                    r_occ[c] = 0;
                    r_o[0] = c;
                    res = TR_WIN;
                    return 1;
                }
                break;
            }
            nc = (uint8_t)(c + dy);
            if (solid_for(nc, kind))
                break;
            k = r_occ[nc];
            if (k) {
                k--;
                if (k == 0) {
                    /* something lands on the mech */
                    if (kind == 2 || dist) {
                        r_o[s] = c;
                        mech_dies(nc, kind == 2 ? DIE_BOT : DIE_CRUSH, s, lg);
                        return 1;
                    }
                    break;
                }
                if (k >= SLOT_BOT) {
                    if (s == 0) {
                        r_o[0] = c;
                        mech_dies(c, DIE_BOT, k, lg);
                        return 1;
                    }
                    if (kind == 1 && dist) {
                        kill_bot(k, nc, s, lg);   /* crushed: the crate keeps falling */
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            }
            r_occ[c] = 0;
            c = nc;
            r_occ[c] = (uint8_t)(s + 1);
            dist++;
            moved = 1;
            if (r_cell[c] == T_PLASMA && kind != 1) {
                if (s == 0) {
                    r_o[0] = c;
                    mech_dies(c, DIE_ZAP, NO_KILLER, lg);
                    return 1;
                }
                r_o[s] = c;
                kill_bot(s, c, NO_KILLER, lg);
                c = NONE;
                break;
            }
            if (s == 0)
                pick_at(c, lg);
        }
        if (c)
            r_o[s] = c;
    }
    return moved;
}

static void occ_clear(void)
{
    uint8_t i;
    for (i = 0; i < NOBJ; i++)
        if (r_o[i])
            r_occ[r_o[i]] = 0;
}

uint8_t ref_sim_core(uint8_t act, TurnLog *lg)
{
    uint8_t i, p, t, t2, k, x, ng, solid_t, pass, d0, b, bx, px;
    int8_t dx;

    for (i = 0; i < NOBJ; i++)
        if (r_o[i])
            r_occ[r_o[i]] = (uint8_t)(i + 1);
    r_doors = calc_doors();
    d0 = r_doors;
    res = TR_OK;
    if (lg) {
        memset(lg, 0, sizeof(TurnLog));
        memset(lg->killer, NO_KILLER, NOBJ);
        lg->act = act;
        memcpy(lg->start, r_o, NOBJ);
    }

    /* ---- 1. the action ---- */
    p = r_o[0];
    if (act == ACT_FLIP) {
        ng = (uint8_t)(r_g ^ 1);
        solid_t = ng == G_DOWN ? T_PHASE_P : T_PHASE_G;
        for (i = 0; i < NOBJ; i++)
            if (r_o[i] && r_cell[r_o[i]] == solid_t)
                goto blocked;           /* a phase block can't form around something */
        r_g = ng;
        if (lg) lg->flags |= EV_FLIP;
    } else {
        dx = act == ACT_L ? -1 : 1;
        x = CX(p);
        if ((dx < 0 && x == 0) || (dx > 0 && x == RW - 1)) {
            if (dx > 0 && p == r_exit && r_exit_side == SIDE_R && r_got == r_full) {
                res = TR_WIN;
                if (lg) {
                    lg->flags |= EV_WIN;
                    memcpy(lg->step, r_o, NOBJ);
                    memcpy(lg->end, r_o, NOBJ);
                }
                occ_clear();
                return TR_WIN;
            }
            goto blocked;
        }
        t = (uint8_t)(p + dx);
        if (solid_for(t, 0))
            goto blocked;
        k = r_occ[t];
        if (k) {
            k--;
            if (k >= SLOT_BOT) {
                /* walked into a drone */
                mech_dies(t, DIE_BOT, k, lg);
                goto done;
            }
            /* a crate: push it if there is room behind it */
            x = CX(t);
            if ((dx < 0 && x == 0) || (dx > 0 && x == RW - 1))
                goto blocked;
            t2 = (uint8_t)(t + dx);
            if (solid_for(t2, 1) || r_occ[t2])
                goto blocked;
            r_occ[t] = 0;
            r_occ[t2] = (uint8_t)(k + 1);
            r_o[k] = t2;
            if (lg) lg->flags |= EV_PUSH;
        }
        r_occ[p] = 0;
        r_occ[t] = 1;
        r_o[0] = t;
        if (r_cell[t] == T_PLASMA) {
            mech_dies(t, DIE_ZAP, NO_KILLER, lg);
            goto done;
        }
        pick_at(t, lg);
        r_doors = calc_doors();
    }

    /* ---- 2. the drones step towards the mech's column, nearest first ---- */
    px = CX(r_o[0]);
    {
        /* order the (at most two) drones: nearest the mech's column first */
        uint8_t ord[MAX_BOTS], m = 0;
        uint8_t a0 = r_o[SLOT_BOT], a1 = r_o[SLOT_BOT + 1];
        if (a0) ord[m++] = SLOT_BOT;
        if (a1) ord[m++] = SLOT_BOT + 1;
        if (m == 2) {
            uint8_t x0 = CX(a0), x1 = CX(a1);
            uint8_t e0 = x0 > px ? (uint8_t)(x0 - px) : (uint8_t)(px - x0);
            uint8_t e1 = x1 > px ? (uint8_t)(x1 - px) : (uint8_t)(px - x1);
            if (e1 < e0 || (e1 == e0 && a1 < a0)) {
                ord[0] = SLOT_BOT + 1;
                ord[1] = SLOT_BOT;
            }
        }
        for (i = 0; i < m; i++) {
            k = ord[i];
            b = r_o[k];
            bx = CX(b);
            if (bx == px) continue;
            t = bx < px ? (uint8_t)(b + 1) : (uint8_t)(b - 1);
            if (t == r_o[0]) {
                /* caught */
                r_occ[b] = 0;
                r_o[k] = t;
                mech_dies(t, DIE_BOT, k, lg);
                goto done;
            }
            if (solid_for(t, 2) || r_occ[t] || r_cell[t] == T_PLASMA)
                continue;
            r_occ[b] = 0;
            r_occ[t] = (uint8_t)(k + 1);
            r_o[k] = t;
        }
        if (m) r_doors = calc_doors();
    }
    if (lg) memcpy(lg->step, r_o, NOBJ);

    /* ---- 3. gravity ---- */
    for (pass = 0; pass < 4; pass++) {
        r_doors = calc_doors();
        fall_pass(lg);
        if (res != TR_OK)
            break;
        if (calc_doors() == r_doors)
            break;
    }
    r_doors = calc_doors();

done:
    if (lg) {
        memcpy(lg->end, r_o, NOBJ);
        lg->doors = r_doors;
        if (r_doors != d0) lg->flags |= EV_DOOR;
        if (res == TR_WIN) lg->flags |= EV_WIN;
        if (res == TR_DEAD) memcpy(lg->end, r_o, NOBJ);
    }
    occ_clear();
    return res;

blocked:
    occ_clear();
    return TR_BLOCKED;
}

uint8_t ref_state_start(const Room *R, State *s)
{
    uint8_t i;
    memset(s, 0, sizeof(State));
    s->o[0] = R->entry;
    for (i = 0; i < MAX_CRATES; i++) s->o[SLOT_CRATE + i] = R->crate0[i];
    for (i = 0; i < MAX_BOTS; i++) s->o[SLOT_BOT + i] = R->bot0[i];
    s->g = R->g0;
    s->got = 0;
    return ref_settle_state(R, s);
}

uint8_t ref_sim_turn(const Room *R, State *s, uint8_t act, TurnLog *log)
{
    uint8_t r;
    ref_sim_load(R);
    memcpy(r_o, s->o, NOBJ);
    r_g = s->g;
    r_got = s->got;
    r = ref_sim_core(act, log);
    if (r != TR_BLOCKED) {
        memcpy(s->o, r_o, NOBJ);
        s->g = r_g;
        s->got = r_got;
    }
    return r;
}

uint8_t ref_doors_open(const Room *R, const State *s)
{
    uint8_t i, c;
    for (i = 0; i < NOBJ; i++) {
        c = s->o[i];
        if (c && R->cell[c] == T_PLATE)
            return 1;
    }
    return 0;
}

uint8_t ref_settle_state(const Room *R, State *s)
{
    uint8_t i, pass;
    ref_sim_load(R);
    memcpy(r_o, s->o, NOBJ);
    r_g = s->g;
    r_got = s->got;
    res = TR_OK;
    for (i = 0; i < NOBJ; i++)
        if (r_o[i])
            r_occ[r_o[i]] = (uint8_t)(i + 1);
    for (pass = 0; pass < 4; pass++) {
        r_doors = calc_doors();
        fall_pass(NULL);
        if (res != TR_OK)
            break;
        if (calc_doors() == r_doors)
            break;
    }
    occ_clear();
    memcpy(s->o, r_o, NOBJ);
    s->g = r_g;
    s->got = r_got;
    return res == TR_DEAD ? TR_DEAD : TR_OK;
}
