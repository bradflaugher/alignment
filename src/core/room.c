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
 * This is the hottest code in the game: the solver runs it three times for every state
 * it visits, thousands of times per room, on a 1 MHz SM83. So it is written for SDCC:
 * state in file-scope globals (stack locals and struct pointers are slow on the SM83),
 * a flag byte per cell so a solidity test is one AND, no multiplies, few calls. The
 * readable reference version is tests/ref_room.c; the host tests check that the two
 * agree on every turn of thousands of random games. Bank 0: the game and the solver
 * both call it. */
#include <string.h>
#include "room.h"

const uint8_t cell_x[RN] = {
    0,1,2,3,4,5,6,7,8,9, 0,1,2,3,4,5,6,7,8,9, 0,1,2,3,4,5,6,7,8,9,
    0,1,2,3,4,5,6,7,8,9, 0,1,2,3,4,5,6,7,8,9, 0,1,2,3,4,5,6,7,8,9,
    0,1,2,3,4,5,6,7,8,9, 0,1,2,3,4,5,6,7,8,9, 0,1,2,3,4,5,6,7,8,9,
};
const uint8_t cell_y[RN] = {
    0,0,0,0,0,0,0,0,0,0, 1,1,1,1,1,1,1,1,1,1, 2,2,2,2,2,2,2,2,2,2,
    3,3,3,3,3,3,3,3,3,3, 4,4,4,4,4,4,4,4,4,4, 5,5,5,5,5,5,5,5,5,5,
    6,6,6,6,6,6,6,6,6,6, 7,7,7,7,7,7,7,7,7,7, 8,8,8,8,8,8,8,8,8,8,
};

/* cell flags */
#define F_WALL    0x01
#define F_PP      0x02    /* solid while gravity is down */
#define F_PG      0x04    /* solid while gravity is up */
#define F_DOOR    0x08    /* solid while the doors are shut */
#define F_DOORWAY 0x10    /* solid to crates and drones */
#define F_PLASMA  0x20
#define F_PICK    0x40
#define F_PLATE   0x80

static const uint8_t t_flags[NUM_T] = {
    0, F_WALL, F_PLASMA, F_PP, F_PG, F_PLATE, F_DOOR, F_PICK, F_PICK, F_PICK,
    F_DOORWAY, F_DOORWAY
};

uint8_t f_o[NOBJ];
uint8_t f_g, f_got;

static uint8_t fl[RN];          /* F_* per cell */
static uint8_t occ[RN];         /* slot + 1 of the object in a cell, 0 = free */
static uint8_t pick_cell[MAX_PICKS];
static uint8_t f_full, f_exit, f_exit_side, f_has_plate;
static uint8_t doors, m_mech, m_obj, res;
static TurnLog *lg;

/* scratch (globals: SDCC keeps them at fixed addresses) */
static uint8_t s_c, s_nc, s_k, s_s, s_mask, s_dist, s_edge;
static int8_t s_dy;

void room_clear(Room *R)
{
    memset(R, 0, sizeof(Room));
    R->featured = 0xFF;
}

uint8_t room_full(const Room *R)
{
    return (uint8_t)((1u << R->npicks) - 1u);
}

void sim_load(const Room *R)
{
    uint8_t i, t;
    const uint8_t *src = R->cell;
    f_has_plate = 0;
    pick_cell[0] = pick_cell[1] = pick_cell[2] = NONE;
    for (i = 0; i < RN; i++) {
        t = src[i];
        fl[i] = t_flags[t];
        if (t == T_PLATE) f_has_plate = 1;
        else if (IS_PICK(t)) pick_cell[t - T_PICK0] = i;
    }
    memset(occ, 0, RN);
    f_full = (uint8_t)((1u << R->npicks) - 1u);
    f_exit = R->exit;
    f_exit_side = R->exit_side;
}

static void set_masks(void)
{
    m_mech = (uint8_t)(F_WALL | (f_g ? F_PG : F_PP) | (doors ? 0 : F_DOOR));
    m_obj = (uint8_t)(m_mech | F_DOORWAY);
}

static void calc_doors(void)
{
    doors = 0;
    if (!f_has_plate) return;
    if ((f_o[0] && (fl[f_o[0]] & F_PLATE)) ||
        (f_o[1] && (fl[f_o[1]] & F_PLATE)) ||
        (f_o[2] && (fl[f_o[2]] & F_PLATE)) ||
        (f_o[3] && (fl[f_o[3]] & F_PLATE)) ||
        (f_o[4] && (fl[f_o[4]] & F_PLATE)))
        doors = 1;
}

static void pick_at(uint8_t c)
{
    uint8_t bit;
    if (c == pick_cell[0]) bit = 1;
    else if (c == pick_cell[1]) bit = 2;
    else bit = 4;
    if (f_got & bit) return;
    f_got |= bit;
    if (lg) {
        lg->picked |= bit;
        lg->flags |= EV_PICK;
        if (f_got == f_full) lg->flags |= EV_EXIT_OPEN;
    }
}

static void kill_bot(uint8_t slot, uint8_t at, uint8_t killer)
{
    occ[f_o[slot]] = 0;
    f_o[slot] = NONE;
    if (lg) {
        lg->died_at[slot] = at;
        lg->killer[slot] = killer;
        lg->flags |= EV_BOT_DIED;
    }
}

static void mech_dies(uint8_t at, uint8_t cause, uint8_t killer)
{
    res = TR_DEAD;
    if (lg) {
        lg->flags |= EV_DEAD;
        lg->cause = cause;
        lg->died_at[0] = at;
        lg->killer[0] = killer;
    }
}

/* Let slot s_s fall from s_c. Sets res on a win or a death. */
static void fall_one(void)
{
    s_mask = s_s ? m_obj : m_mech;
    s_dist = 0;
    for (;;) {
        if (cell_y[s_c] == s_edge) {
            /* off the screen: only the mech, only out of an open exit */
            if (!s_s && s_c == f_exit && f_got == f_full &&
                f_exit_side == (f_g ? SIDE_T : SIDE_B)) {
                f_o[0] = s_c;
                res = TR_WIN;
                return;
            }
            break;
        }
        s_nc = (uint8_t)(s_c + s_dy);
        if (fl[s_nc] & s_mask)
            break;
        s_k = occ[s_nc];
        if (s_k) {
            s_k--;
            if (!s_k) {
                /* something lands on the mech: a drone always, a crate if it fell */
                if (s_s >= SLOT_BOT || s_dist) {
                    f_o[s_s] = s_c;
                    mech_dies(s_nc, s_s >= SLOT_BOT ? DIE_BOT : DIE_CRUSH, s_s);
                    return;
                }
                break;
            }
            if (s_k >= SLOT_BOT) {
                if (!s_s) {
                    f_o[0] = s_c;
                    mech_dies(s_c, DIE_BOT, s_k);
                    return;
                }
                if (s_s < SLOT_BOT && s_dist)
                    kill_bot(s_k, s_nc, s_s);   /* crushed: the crate keeps falling */
                else
                    break;
            } else {
                break;
            }
        }
        occ[s_c] = 0;
        s_c = s_nc;
        occ[s_c] = (uint8_t)(s_s + 1);
        s_dist++;
        if (fl[s_c] & (F_PLASMA | F_PICK)) {
            if (fl[s_c] & F_PLASMA) {
                if (!s_s) {
                    f_o[0] = s_c;
                    mech_dies(s_c, DIE_ZAP, NO_KILLER);
                    return;
                }
                if (s_s >= SLOT_BOT) {
                    f_o[s_s] = s_c;
                    kill_bot(s_s, s_c, NO_KILLER);
                    return;
                }
            } else if (!s_s) {
                pick_at(s_c);
            }
        }
    }
    f_o[s_s] = s_c;
}

static void fall_pass(void)
{
    uint8_t order[NOBJ], key[NOBJ];
    uint8_t n = 0, i, j, c, kk;
    s_dy = f_g ? -RW : RW;
    s_edge = f_g ? 0 : RH - 1;
    /* the object nearest the ground falls first */
    for (i = 0; i < NOBJ; i++) {
        c = f_o[i];
        if (!c) continue;
        kk = f_g ? cell_y[c] : (uint8_t)(RH - 1 - cell_y[c]);
        j = n;
        while (j && key[j - 1] > kk) {
            key[j] = key[j - 1];
            order[j] = order[j - 1];
            j--;
        }
        key[j] = kk;
        order[j] = i;
        n++;
    }
    for (i = 0; i < n; i++) {
        s_s = order[i];
        s_c = f_o[s_s];
        if (!s_c) continue;             /* crushed earlier in this pass */
        fall_one();
        if (res != TR_OK) return;
    }
}

static void occ_clear(void)
{
    if (f_o[0]) occ[f_o[0]] = 0;
    if (f_o[1]) occ[f_o[1]] = 0;
    if (f_o[2]) occ[f_o[2]] = 0;
    if (f_o[3]) occ[f_o[3]] = 0;
    if (f_o[4]) occ[f_o[4]] = 0;
}

/* the drones step towards the mech's column, the nearest one first */
static uint8_t bots_step(void)
{
    uint8_t px = cell_x[f_o[0]], a0 = f_o[SLOT_BOT], a1 = f_o[SLOT_BOT + 1];
    uint8_t first = SLOT_BOT, second = SLOT_BOT + 1, i, k, b, bx, t, e0, e1;
    if (a0 && a1) {
        bx = cell_x[a0];
        e0 = bx > px ? (uint8_t)(bx - px) : (uint8_t)(px - bx);
        bx = cell_x[a1];
        e1 = bx > px ? (uint8_t)(bx - px) : (uint8_t)(px - bx);
        if (e1 < e0 || (e1 == e0 && a1 < a0)) {
            first = SLOT_BOT + 1;
            second = SLOT_BOT;
        }
    }
    for (i = 0; i < 2; i++) {
        k = i ? second : first;
        b = f_o[k];
        if (!b) continue;
        bx = cell_x[b];
        if (bx == px) continue;
        t = bx < px ? (uint8_t)(b + 1) : (uint8_t)(b - 1);
        if (t == f_o[0]) {
            /* caught */
            occ[b] = 0;
            f_o[k] = t;
            mech_dies(t, DIE_BOT, k);
            return 1;
        }
        if ((fl[t] & (m_obj | F_PLASMA)) || occ[t])
            continue;
        occ[b] = 0;
        occ[t] = (uint8_t)(k + 1);
        f_o[k] = t;
    }
    return 0;
}

uint8_t sim_core(uint8_t act, TurnLog *log)
{
    uint8_t p, t, t2, k, d0, pass, d1, m;

    lg = log;
    if (f_o[0]) occ[f_o[0]] = 1;
    if (f_o[1]) occ[f_o[1]] = 2;
    if (f_o[2]) occ[f_o[2]] = 3;
    if (f_o[3]) occ[f_o[3]] = 4;
    if (f_o[4]) occ[f_o[4]] = 5;
    calc_doors();
    d0 = doors;
    set_masks();
    res = TR_OK;
    if (lg) {
        memset(lg, 0, sizeof(TurnLog));
        memset(lg->killer, NO_KILLER, NOBJ);
        lg->act = act;
        memcpy(lg->start, f_o, NOBJ);
    }

    /* ---- 1. the action ---- */
    p = f_o[0];
    if (act == ACT_FLIP) {
        /* a phase block can't form around something */
        m = f_g ? F_PP : F_PG;
        if ((f_o[0] && (fl[f_o[0]] & m)) || (f_o[1] && (fl[f_o[1]] & m)) ||
            (f_o[2] && (fl[f_o[2]] & m)) || (f_o[3] && (fl[f_o[3]] & m)) ||
            (f_o[4] && (fl[f_o[4]] & m)))
            goto blocked;
        f_g ^= 1;
        set_masks();
        if (lg) lg->flags |= EV_FLIP;
    } else {
        if (act == ACT_L) {
            if (!cell_x[p]) goto blocked;
            t = (uint8_t)(p - 1);
        } else {
            if (cell_x[p] == RW - 1) {
                if (p == f_exit && f_exit_side == SIDE_R && f_got == f_full) {
                    if (lg) {
                        lg->flags |= EV_WIN;
                        memcpy(lg->step, f_o, NOBJ);
                        memcpy(lg->end, f_o, NOBJ);
                    }
                    occ_clear();
                    return TR_WIN;
                }
                goto blocked;
            }
            t = (uint8_t)(p + 1);
        }
        if (fl[t] & m_mech)
            goto blocked;
        k = occ[t];
        if (k) {
            k--;
            if (k >= SLOT_BOT) {
                /* walked into a drone */
                mech_dies(t, DIE_BOT, k);
                goto done;
            }
            /* a crate (never on the border): push it if there is room behind it */
            t2 = act == ACT_L ? (uint8_t)(t - 1) : (uint8_t)(t + 1);
            if ((fl[t2] & m_obj) || occ[t2])
                goto blocked;
            occ[t] = 0;
            occ[t2] = (uint8_t)(k + 1);
            f_o[k] = t2;
            if (lg) lg->flags |= EV_PUSH;
        }
        occ[p] = 0;
        occ[t] = 1;
        f_o[0] = t;
        if (fl[t] & (F_PLASMA | F_PICK)) {
            if (fl[t] & F_PLASMA) {
                mech_dies(t, DIE_ZAP, NO_KILLER);
                goto done;
            }
            pick_at(t);
        }
        if (f_has_plate) {
            calc_doors();
            set_masks();
        }
    }

    /* ---- 2. the drones ---- */
    if (f_o[SLOT_BOT] || f_o[SLOT_BOT + 1]) {
        if (bots_step()) goto done;
        if (f_has_plate) {
            calc_doors();
            set_masks();
        }
    }
    if (lg) memcpy(lg->step, f_o, NOBJ);

    /* ---- 3. gravity ---- */
    for (pass = 0; pass < 4; pass++) {
        calc_doors();
        set_masks();
        d1 = doors;
        fall_pass();
        if (res != TR_OK) break;
        if (!f_has_plate) break;
        calc_doors();
        if (doors == d1) break;
    }
    calc_doors();

done:
    if (lg) {
        memcpy(lg->end, f_o, NOBJ);
        lg->doors = doors;
        if (doors != d0) lg->flags |= EV_DOOR;
        if (res == TR_WIN) lg->flags |= EV_WIN;
    }
    occ_clear();
    return res;

blocked:
    occ_clear();
    return TR_BLOCKED;
}

uint8_t state_start(const Room *R, State *s)
{
    uint8_t i;
    memset(s, 0, sizeof(State));
    s->o[0] = R->entry;
    for (i = 0; i < MAX_CRATES; i++) s->o[SLOT_CRATE + i] = R->crate0[i];
    for (i = 0; i < MAX_BOTS; i++) s->o[SLOT_BOT + i] = R->bot0[i];
    s->g = R->g0;
    s->got = 0;
    return settle_state(R, s);
}

uint8_t sim_turn(const Room *R, State *s, uint8_t act, TurnLog *log)
{
    uint8_t r;
    sim_load(R);
    memcpy(f_o, s->o, NOBJ);
    f_g = s->g;
    f_got = s->got;
    r = sim_core(act, log);
    if (r != TR_BLOCKED) {
        memcpy(s->o, f_o, NOBJ);
        s->g = f_g;
        s->got = f_got;
    }
    return r;
}

uint8_t doors_open(const Room *R, const State *s)
{
    uint8_t i, c;
    for (i = 0; i < NOBJ; i++) {
        c = s->o[i];
        if (c && R->cell[c] == T_PLATE)
            return 1;
    }
    return 0;
}

uint8_t settle_state(const Room *R, State *s)
{
    uint8_t i, pass, d1;
    sim_load(R);
    memcpy(f_o, s->o, NOBJ);
    f_g = s->g;
    f_got = s->got;
    lg = NULL;
    res = TR_OK;
    for (i = 0; i < NOBJ; i++)
        if (f_o[i])
            occ[f_o[i]] = (uint8_t)(i + 1);
    for (pass = 0; pass < 4; pass++) {
        calc_doors();
        set_masks();
        d1 = doors;
        fall_pass();
        if (res != TR_OK) break;
        calc_doors();
        if (doors == d1) break;
    }
    occ_clear();
    memcpy(s->o, f_o, NOBJ);
    s->g = f_g;
    s->got = f_got;
    return res == TR_DEAD ? TR_DEAD : TR_OK;
}
