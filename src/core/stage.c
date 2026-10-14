/* ALIGNMENT - the stage generator.
 *
 * A stage is a corridor through the station, generated left to right as a run of
 * sections. At every column the corridor has a ceiling (solid rows 0..c) and a floor
 * (solid rows f..15); the mech runs along whichever one gravity holds it to. Sections
 * bend and shift that corridor and put things in it: plasma pits in the floor or the
 * ceiling (flip to cross), staggered pits (flip in mid-air), shafts that move the whole
 * corridor up or down (only reachable from the ceiling, or only from the floor), phase
 * walls and bridges that exist in one gravity only, crawler and hover drones, laser
 * gates and crushers.
 *
 * Every stage is then run through stage_check(): if the exit can't be reached, the
 * section where progress stopped is rebuilt as a plain one and the stage is generated
 * again, so a stage is always passable. Everything comes from the run seed and the
 * stage number through the shared rng, so the Game Boy and the host make identical
 * stages. */
#ifdef __SDCC
#pragma bank 255
#endif
#include <string.h>
#include "stage.h"
#include "rng.h"

#ifdef __SDCC
#include <gb/gb.h>
#define WORK ((uint16_t *)0xA000)       /* the checker's queue lives in cartridge RAM bank 1 */
#define WORK_MAP() do { ENABLE_RAM; SWITCH_RAM(1); } while (0)
#else
static uint16_t work_q[SW_MAX * SH * 2];
#define WORK work_q
#define WORK_MAP() do { } while (0)
#endif

uint8_t check_seen[SW_MAX * SH * 2 / 8];

/* ---------------------------------------------------------------- unlocks */

/* stage where each mechanic first appears */
static const uint8_t unlock_at[NUM_MECH] = {
    1,  /* M_PITS */
    1,  /* M_PLASMA */
    3,  /* M_STAGGER */
    4,  /* M_CRAWLER */
    5,  /* M_PHASE */
    6,  /* M_HOVER */
    7,  /* M_LASER */
    8,  /* M_CRUSHER */
    2,  /* M_SHAFT */
    9,  /* M_TURRET */
};

uint8_t stage_new_mech(uint16_t n) CORE_BANKED
{
    uint8_t m;
    if (n == 1) return M_PITS;
    for (m = 0; m < NUM_MECH; m++)
        if (unlock_at[m] == n) return m;
    return 0xFF;
}

/* ---------------------------------------------------------------- building */

static Stage *S;
static uint8_t gx;          /* next column to build */
static uint8_t gc, gf;      /* corridor: ceiling row (solid up to it), floor row (solid from it) */
static uint8_t lvl;         /* difficulty 0..30 */
static uint32_t easy_mask;  /* sections to build plain (repairs) */
static uint8_t sec;         /* section counter */
static uint8_t sec_start[40];
static uint8_t nsec;

static uint8_t rnd(uint8_t n) { return rng_range(n); }

static void put_col(uint8_t x, uint8_t c, uint8_t f)
{
    uint8_t y;
    uint8_t *col = &S->cell[SCELL(x, 0)];
    for (y = 0; y < SH; y++)
        col[y] = (y <= c || y >= f) ? T_WALL : T_EMPTY;
}

static void set(uint8_t x, uint8_t y, uint8_t t)
{
    if (x < S->w && y < SH) S->cell[SCELL(x, y)] = t;
}

static uint8_t get(uint8_t x, uint8_t y)
{
    return S->cell[SCELL(x, y)];
}

/* plain corridor columns */
static void flat(uint8_t len)
{
    while (len-- && gx < S->w) put_col(gx++, gc, gf);
}

static void add_enemy(uint8_t kind, uint8_t x0, uint8_t x1, uint8_t y0, uint8_t y1, uint8_t speed)
{
    Enemy *e;
    if (S->nenemies >= MAX_ENEMIES) return;
    e = &S->enemy[S->nenemies++];
    e->kind = kind;
    e->x0 = x0;
    e->x1 = x1;
    e->y0 = y0;
    e->y1 = y1;
    e->speed = speed;
    e->phase = rnd(255);
    e->hp = enemy_hp(kind);
}

uint8_t enemy_hp(uint8_t kind) CORE_BANKED
{
    static const uint8_t hp[NUM_E] = { 2, 1, HP_INVULNERABLE, 3 };
    return hp[kind];
}

void stage_rearm(Stage *st) CORE_BANKED
{
    uint8_t i;
    Enemy *e = st->enemy;
    for (i = st->nenemies; i; i--, e++) e->hp = enemy_hp(e->kind);
}

static void door_rows(Stage *st, uint8_t x, uint16_t mask, uint8_t t)
{
    uint8_t y;
    uint8_t *col = &st->cell[SCELL(x, 0)];
    for (y = 0; y < SH; y++, mask >>= 1)
        if (mask & 1) col[y] = t;
}

void arena_reset(Stage *st) CORE_BANKED
{
    if (st->arena.kind == BOSS_NONE) return;
    door_rows(st, st->arena.x0, st->arena.m0, T_DOOR_OPEN);
    door_rows(st, st->arena.x1, st->arena.m1, T_DOOR);
}

void arena_seal(Stage *st) CORE_BANKED
{
    if (st->arena.kind == BOSS_NONE) return;
    door_rows(st, st->arena.x0, st->arena.m0, T_DOOR);
}

void arena_open(Stage *st) CORE_BANKED
{
    if (st->arena.kind == BOSS_NONE) return;
    door_rows(st, st->arena.x1, st->arena.m1, T_EMPTY);
}

/* a turret's bolt runs along its row until the first solid cell (at most 15 cells) */
static uint8_t turret_reach(uint8_t x, uint8_t y, int8_t dir)
{
    uint8_t k;
    for (k = 1; k < 15; k++) {
        uint8_t xx = (uint8_t)(x + dir * k), t;
        if (xx >= S->w) break;
        t = S->cell[SCELL(xx, y)];
        if (t == T_WALL || t == T_PHASE_P || t == T_PHASE_G || t == T_DOOR) break;
    }
    return (uint8_t)(x + dir * (k - 1));
}

/* the corridor's height, kept in 4..8 (3 in places later on) */
static void bend(void)
{
    uint8_t k = rnd(4);
    if (k == 0 && gf - gc - 1 > 4 && gf > 2) gf--;          /* floor steps up */
    else if (k == 1 && gf < SH - 2 && gf - gc - 1 < 8) gf++; /* floor steps down */
    else if (k == 2 && gc > 1 && gf - gc - 1 < 8) gc--;      /* ceiling steps up */
    else if (k == 3 && gf - gc - 1 > 4) gc++;                /* ceiling steps down */
}

/* ---- sections ---- */

static void sec_steps(void)
{
    uint8_t n = (uint8_t)(2 + rnd(3)), i;
    for (i = 0; i < n; i++) {
        flat((uint8_t)(1 + rnd(3)));
        bend();
    }
    flat(1);
}

/* a pit of plasma in the floor (or the ceiling): jump a short one, flip over a long one */
static void sec_pit(uint8_t ceiling)
{
    uint8_t w = (uint8_t)(2 + rnd(2) + lvl / 6), i;
    if (w > 7) w = 7;
    flat(2);
    for (i = 0; i < w && gx < S->w; i++) {
        put_col(gx, gc, gf);
        set(gx, ceiling ? gc : gf, T_PLASMA);
        gx++;
    }
    flat(2);
}

/* floor pit, then a ceiling pit right after it, and so on: flip at the last moment
 * and drift over the seam in mid-air */
static void sec_stagger(void)
{
    uint8_t n = (uint8_t)(2 + rnd(2) + (lvl > 12)), i, j, w, side = rnd(2);
    flat(2);
    for (i = 0; i < n; i++) {
        w = (uint8_t)(3 + rnd(3));
        for (j = 0; j < w && gx < S->w; j++) {
            put_col(gx, gc, gf);
            set(gx, side ? gc : gf, T_PLASMA);
            gx++;
        }
        side ^= 1;
    }
    flat(2);
}

/* the corridor jumps up or down by 3-5 rows: a ledge only one side of gravity can use */
static void sec_shaft(void)
{
    /* the corridor first opens up to 7 rows, then moves by up to 5: the old and the new
     * corridor must overlap, or there is no way through */
    uint8_t k = (uint8_t)(3 + rnd(3)), h;
    flat(2);
    while (gf - gc - 1 < 7) {
        if (gc > 1) gc--;
        else if (gf < SH - 1) gf++;
        else break;
        put_col(gx++, gc, gf);
    }
    h = (uint8_t)(gf - gc - 1);
    if (k > h - 2) k = (uint8_t)(h - 2);
    if ((rnd(2) && gc > k) || gf + k > SH - 1) {
        /* up: fall upwards into it */
        if (gc < k + 1) k = (uint8_t)(gc - 1);
        gc = (uint8_t)(gc - k);
        gf = (uint8_t)(gf - k);
    } else {
        if (gf + k > SH - 1) k = (uint8_t)(SH - 1 - gf);
        gf = (uint8_t)(gf + k);
        gc = (uint8_t)(gc + k);
    }
    flat((uint8_t)(3 + rnd(2)));
    /* a little plasma at the foot of the drop, now and then */
    if (lvl > 4 && rnd(2)) {
        set((uint8_t)(gx - 1), gf, T_PLASMA);
    }
    flat(2);
    /* back to a normal height */
    while (gf - gc - 1 > 6) {
        if (rnd(2)) gc++;
        else gf--;
        put_col(gx++, gc, gf);
    }
}

static void sec_crawler(void)
{
    uint8_t len = (uint8_t)(6 + rnd(4)), x0 = gx, top = rnd(2);
    flat(len);
    add_enemy(E_CRAWLER, (uint8_t)(x0 + 1), (uint8_t)(x0 + len - 2), top ? (uint8_t)(gc + 1) : (uint8_t)(gf - 1),
              top, (uint8_t)(1 + (lvl > 10) + (lvl > 20)));
    flat(2);
}

static void sec_hover(void)
{
    uint8_t x;
    flat(3);
    x = gx;
    flat(3);
    add_enemy(E_HOVER, (uint8_t)(x + 1), (uint8_t)(x + 1), (uint8_t)(gc + 1), (uint8_t)(gf - 1),
              (uint8_t)(1 + (lvl > 14)));
    flat(2);
}

static void sec_crusher(void)
{
    uint8_t x;
    flat(3);
    x = gx;
    flat(2);
    add_enemy(E_CRUSHER, (uint8_t)(x + 1), (uint8_t)(x + 1), (uint8_t)(gc + 1), (uint8_t)(gf - 1),
              (uint8_t)(1 + (lvl > 16)));
    flat(3);
}

static void sec_laser(void)
{
    uint8_t y;
    flat(3);
    put_col(gx, gc, gf);
    for (y = (uint8_t)(gc + 1); y < gf; y++) set(gx, y, T_LASER);
    gx++;
    flat(3);
}

/* a gun turret on the floor or the ceiling, firing back down the corridor */
static void sec_turret(void)
{
    uint8_t x, top = rnd(3) == 0, y;
    flat((uint8_t)(4 + rnd(3)));
    x = gx;
    flat(1);
    y = top ? (uint8_t)(gc + 1) : (uint8_t)(gf - 1);
    add_enemy(E_TURRET, turret_reach(x, y, -1), x, y, top, 1);
    flat(2);
}

/* phase: a wall that only one gravity lets through, or a bridge only one gravity holds */
static void sec_phase(void)
{
    uint8_t y, i, k = rnd(3), w, t;
    flat(2);
    if (k == 0) {
        /* two walls of opposite colours: down through one, up through the other */
        t = rnd(2) ? T_PHASE_P : T_PHASE_G;
        for (i = 0; i < 2; i++) {
            put_col(gx, gc, gf);
            for (y = (uint8_t)(gc + 1); y < gf; y++) set(gx, y, t);
            gx++;
            flat((uint8_t)(3 + rnd(2)));
            t = t == T_PHASE_P ? T_PHASE_G : T_PHASE_P;
        }
    } else {
        /* a bridge over plasma: purple on the floor (walk it with gravity down) or green
         * under the ceiling (walk it upside down), with plasma on the other side */
        w = (uint8_t)(3 + rnd(3));
        t = k == 1;     /* 1: floor bridge */
        for (i = 0; i < w && gx < S->w; i++) {
            put_col(gx, gc, gf);
            if (t) {
                set(gx, gf, T_PHASE_P);
                set(gx, (uint8_t)(gf + 1), T_PLASMA);
                if (i > 0 && i < w - 1) set(gx, gc, T_PLASMA);
            } else {
                set(gx, gc, T_PHASE_G);
                set(gx, (uint8_t)(gc - 1), T_PLASMA);
                if (i > 0 && i < w - 1) set(gx, gf, T_PLASMA);
            }
            gx++;
        }
    }
    flat(2);
}

/* a tall chamber over plasma with floating islands: flip from one to the next */
static void sec_islands(void)
{
    uint8_t c0, f0, w, i, j, x, y, n, iw, top;
    flat(1);
    c0 = gc;
    f0 = gf;
    /* open the chamber up to 9-10 rows */
    while (gf - gc - 1 < 9 && (gc > 1 || gf < SH - 1)) {
        if (gc > 1) gc--;
        if (gf - gc - 1 < 9 && gf < SH - 1) gf++;
    }
    n = (uint8_t)(2 + rnd(2) + (lvl > 10));
    w = (uint8_t)(n * 4 + 2);
    x = gx;
    for (i = 0; i < w && gx < S->w; i++) {
        put_col(gx, gc, gf);
        if (i > 0 && i < w - 1) {
            set(gx, gf, T_PLASMA);
            if (lvl > 6 && (i & 3) == 2) set(gx, gc, T_PLASMA);
        }
        gx++;
    }
    /* islands: alternately high and low, 2-3 cells wide */
    top = rnd(2);
    for (i = 0; i < n; i++) {
        iw = (uint8_t)(2 + rnd(2));
        y = top ? (uint8_t)(gc + 2 + rnd(2)) : (uint8_t)(gf - 3 - rnd(2));
        for (j = 0; j < iw; j++) set((uint8_t)(x + 2 + i * 4 + j), y, T_WALL);
        top ^= 1;
    }
    flat(1);
    /* back to the old corridor */
    while (gc < c0 && gf - gc - 1 > 5) { gc++; put_col(gx++, gc, gf); }
    while (gf > f0 && gf - gc - 1 > 5) { gf--; put_col(gx++, gc, gf); }
    flat(1);
}

/* pillars up from the floor and down from the ceiling, in turn */
static void sec_pillars(void)
{
    uint8_t n = (uint8_t)(3 + rnd(3)), i, hgt, y, h = (uint8_t)(gf - gc - 1);
    flat(1);
    for (i = 0; i < n && gx < S->w; i++) {
        flat((uint8_t)(1 + rnd(3)));
        put_col(gx, gc, gf);
        hgt = (uint8_t)(1 + rnd(h > 5 ? 3 : 2));
        if (hgt > h - 2) hgt = (uint8_t)(h - 2);
        for (y = 0; y < hgt; y++) {
            if (i & 1) set(gx, (uint8_t)(gc + 1 + y), T_WALL);
            else set(gx, (uint8_t)(gf - 1 - y), T_WALL);
        }
        /* a plasma cap now and then */
        if (lvl > 5 && rnd(3) == 0)
            set(gx, (i & 1) ? (uint8_t)(gc + 1 + hgt) : (uint8_t)(gf - 1 - hgt), T_PLASMA);
        gx++;
    }
    flat(1);
}

/* ---- the stage ---- */

static uint8_t pick_section(uint16_t mechs)
{
    /* weighted choice among the unlocked kinds */
    static const uint8_t weight[NUM_MECH] = { 5, 0, 3, 3, 3, 2, 2, 2, 3, 3 };
    uint8_t total = 0, m, r;
    for (m = 0; m < NUM_MECH; m++)
        if (mechs & MBIT(m)) total = (uint8_t)(total + weight[m]);
    total = (uint8_t)(total + 2 + 3 + 3);   /* plain steps, pillars, islands */
    r = rnd(total);
    for (m = 0; m < NUM_MECH; m++) {
        if (!(mechs & MBIT(m))) continue;
        if (r < weight[m]) return m;
        r = (uint8_t)(r - weight[m]);
    }
    if (r < 2) return 0xFF;
    return r < 5 ? 0xFE : 0xFD;
}

static void build_section(uint8_t m, uint16_t *used)
{
    if (sec < 40) sec_start[sec] = gx;
    if (easy_mask & ((uint32_t)1 << (sec & 31))) m = 0xFF;
    sec++;
    if (m < NUM_MECH) *used |= MBIT(m);
    switch (m) {
    case M_PITS: sec_pit(rnd(4) == 0); break;
    case M_STAGGER: sec_stagger(); break;
    case M_CRAWLER: sec_crawler(); break;
    case M_PHASE: sec_phase(); break;
    case M_HOVER: sec_hover(); break;
    case M_LASER: sec_laser(); break;
    case M_CRUSHER: sec_crusher(); break;
    case M_SHAFT: sec_shaft(); break;
    case M_TURRET: sec_turret(); break;
    case 0xFE: sec_pillars(); break;
    case 0xFD: sec_islands(); break;
    default: sec_steps(); break;
    }
}

/* every fourth stage ends in a boss arena: a tall closed room between two blast doors */
#define ARENA_TOP 2
#define ARENA_BOT 13
#define ARENA_W 12
static void build_arena(uint16_t n)
{
    Arena *a = &S->arena;
    uint8_t k = (uint8_t)(((n >> 2) - 1) & 3), y;
    flat(2);
    /* ease the corridor to a 4-row opening at rows 9..12 (the arena's floor is row 13) */
    while (gf != ARENA_BOT || gc != ARENA_BOT - 5) {
        if (gf < ARENA_BOT) gf++;
        else if (gf > ARENA_BOT) gf--;
        if (gc < ARENA_BOT - 5 && gc + 1 < gf - 4) gc++;
        else if (gc > ARENA_BOT - 5) gc--;
        flat(1);
    }
    flat(1);
    a->kind = (uint8_t)(BK_SENTINEL + k);
    a->level = (uint8_t)(n >= 16 ? 2 : 1);
    a->top = ARENA_TOP;
    a->bot = ARENA_BOT;
    a->x0 = gx;
    a->m0 = a->m1 = 0;
    put_col(gx, gc, gf);
    for (y = (uint8_t)(gc + 1); y < gf; y++) {
        set(gx, y, T_DOOR_OPEN);
        a->m0 |= (uint16_t)1 << y;
    }
    gx++;
    gc = ARENA_TOP;
    gf = ARENA_BOT;
    flat(ARENA_W);
    a->x1 = gx;
    put_col(gx, gc, gf);
    for (y = (uint8_t)(gc + 1); y < gf; y++) {
        set(gx, y, T_DOOR);
        a->m1 |= (uint16_t)1 << y;
    }
    gx++;
    /* where the boss starts */
    switch (a->kind) {
    case BK_PISTON: a->bx = (uint8_t)(a->x0 + ARENA_W / 2); a->by = ARENA_TOP + 1; break;
    case BK_HIVE: a->bx = (uint8_t)(a->x0 + ARENA_W / 2); a->by = ARENA_TOP + 3; break;
    default: a->bx = (uint8_t)(a->x1 - 3); a->by = ARENA_BOT - 2; break;
    }
    flat(2);
}

static void build(uint16_t seed, uint16_t n)
{
    uint16_t un = 0, used = 0;
    uint8_t m, feat, half_done = 0, y, boss;
    rng_seed2(seed, n);
    memset(S->cell, T_WALL, sizeof(S->cell));
    S->nenemies = 0;
    S->ncells = 0;
    S->index = n;
    lvl = n > 30 ? 30 : (uint8_t)n;
    S->w = (uint8_t)(96 + (lvl > 16 ? 32 : lvl * 2));
    S->deck = (uint8_t)((n - 1) / 4);
    for (m = 0; m < NUM_MECH; m++)
        if (n >= unlock_at[m]) un |= MBIT(m);
    feat = stage_new_mech(n);
    S->featured = feat;

    gc = (uint8_t)(3 + rnd(3));
    gf = (uint8_t)(gc + 6 + rnd(2));
    gx = 1;
    sec = 0;
    /* the start: a quiet platform */
    flat(5);
    S->start_x = 2;
    S->start_y = (uint8_t)(gf - 1);
    /* a new mechanic gets the first sections to itself */
    if (feat != 0xFF && feat != M_PLASMA) {
        build_section(feat, &used);
        flat(3);
        build_section(feat, &used);
    }
    S->sector = 0;
    memset(&S->arena, 0, sizeof(S->arena));
    boss = (uint8_t)((n & 3) == 0);
    while (gx < S->w - (boss ? 52 : 30)) {     /* (a section is at most ~26 columns) */
        if (!half_done && gx >= S->w / 2) {
            /* the checkpoint: a quiet spot halfway */
            half_done = 1;
            flat(2);
            S->check_x = gx;
            S->check_y = (uint8_t)(gf - 1);
            flat(3);
            set(S->check_x, S->check_y, T_CHECK);
            continue;
        }
        build_section(pick_section(un), &used);
    }
    if (boss) build_arena(n);
    /* the end: a platform and the exit gate across the corridor */
    flat((uint8_t)(S->w - 3 - gx));
    S->exit_x = gx;
    S->exit_y = (uint8_t)(gf - 1);
    put_col(gx, gc, gf);
    for (y = (uint8_t)(gc + 1); y < gf; y++) set(gx, y, T_EXIT);
    gx++;
    flat(1);
    while (gx < S->w) {
        uint8_t *col = &S->cell[SCELL(gx, 0)];
        memset(col, T_WALL, SH);
        gx++;
    }
    nsec = sec;
    S->mechs = used;
    /* enemies left to right (turrets reach back past ones placed before them) */
    {
        uint8_t i, j;
        for (i = 1; i < S->nenemies; i++) {
            Enemy e;
            e = S->enemy[i];
            for (j = i; j > 0 && S->enemy[j - 1].x0 > e.x0; j--) S->enemy[j] = S->enemy[j - 1];
            S->enemy[j] = e;
        }
    }
}

/* energy cells on spots the mech can reach: mostly the far side of the corridor */
static void place_cells(void)
{
    uint8_t x, y, k, tries;
    for (k = 0; k < MAX_CELLS && k < S->w / 5; k++) {
        for (tries = 0; tries < 8; tries++) {
            x = (uint8_t)(6 + rnd((uint8_t)(S->w - 12)));
            y = (uint8_t)(1 + rnd(SH - 2));
            if (get(x, y) != T_EMPTY) continue;
            {
                uint16_t i = SCELL(x, y);
                if (!(check_seen[i >> 3] & (1u << (i & 7))) &&
                    !(check_seen[(i + SW_MAX * SH) >> 3] & (1u << ((i + SW_MAX * SH) & 7))))
                    continue;
            }
            set(x, y, T_CELL);
            S->ncells++;
            break;
        }
    }
}

#ifndef __SDCC
/* host debugging: the stage as rolled, before any repair */
void stage_gen_raw(Stage *stage, uint16_t seed, uint16_t n)
{
    S = stage;
    easy_mask = 0;
    build(seed, n);
    stage_check(S);
}
#endif

void stage_gen(Stage *stage, uint16_t seed, uint16_t n) CORE_BANKED
{
    uint8_t tries, i, xr;
    S = stage;
    easy_mask = 0;
    S->fixes = 0;
    for (tries = 0; tries < 12; tries++) {
        build(seed, n);
        if (stage_check(S)) {
            place_cells();
            return;
        }
        /* rebuild the section where progress stopped as a plain one */
        xr = 0;
        {
            uint16_t c;
            for (c = 0; c < (uint16_t)S->w * SH; c++)
                if ((check_seen[c >> 3] & (1u << (c & 7))) ||
                    (check_seen[(c + SW_MAX * SH) >> 3] & (1u << ((c + SW_MAX * SH) & 7))))
                    if ((uint8_t)(c / SH) > xr) xr = (uint8_t)(c / SH);
        }
        for (i = nsec; i > 0; i--)
            if (i - 1 < 40 && sec_start[i - 1] <= xr + 1) break;
        if (i) easy_mask |= (uint32_t)1 << ((i - 1) & 31);
        if (i > 1) easy_mask |= (uint32_t)1 << ((i - 2) & 31);
        S->fixes++;
    }
    /* never reached in testing: everything plain */
    easy_mask = 0xFFFFFFFFu;
    build(seed, n);
    stage_check(S);
    place_cells();
}

/* ---------------------------------------------------------------- checker */

static const Stage *C;
static uint16_t qn;

/* solid for the mech in gravity g */
static uint8_t solid(uint8_t x, uint8_t y, uint8_t g)
{
    uint8_t t;
    if (x >= C->w || y >= SH) return 1;
    t = C->cell[SCELL(x, y)];
    return (uint8_t)(t == T_WALL || (t == T_PHASE_P && g == G_DOWN) || (t == T_PHASE_G && g == G_UP));
}

static uint8_t deadly(uint8_t x, uint8_t y)
{
    return (uint8_t)(C->cell[SCELL(x, y)] == T_PLASMA);
}

static void visit(uint8_t x, uint8_t y, uint8_t g)
{
    uint16_t i = (uint16_t)(SCELL(x, y) + (g ? SW_MAX * SH : 0));
    uint8_t b = (uint8_t)(1u << (i & 7));
    if (check_seen[i >> 3] & b) return;
    check_seen[i >> 3] |= b;
    WORK[qn++] = i;
}

/* fall from (x, y) in gravity g; returns the resting row or 0xFF (plasma) */
static uint8_t fall(uint8_t x, uint8_t y, uint8_t g)
{
    int8_t d = g ? -1 : 1;
    for (;;) {
        if (deadly(x, y)) return 0xFF;
        if (solid(x, (uint8_t)(y + d), g)) return y;
        y = (uint8_t)(y + d);
    }
}

static void try_land(uint8_t x, uint8_t y, uint8_t g)
{
    uint8_t r;
    if (x >= C->w || solid(x, y, g) || deadly(x, y)) return;
    r = fall(x, y, g);
    if (r != 0xFF) visit(x, r, g);
}

uint8_t stage_check(const Stage *st) CORE_BANKED
{
    uint16_t qh = 0, i;
    uint8_t x, y, g, d, nx, k, r, ng;
    int8_t dir, up;
    C = st;
    WORK_MAP();
    memset(check_seen, 0, sizeof(check_seen));
    qn = 0;
    visit(st->start_x, st->start_y, G_DOWN);
    while (qh < qn) {
        i = WORK[qh++];
        g = (uint8_t)(i >= SW_MAX * SH);
        if (g) i -= SW_MAX * SH;
        x = (uint8_t)(i / SH);
        y = (uint8_t)(i % SH);
        if (x == st->exit_x) return 1;
        up = g ? 1 : -1;            /* "up" relative to gravity */
        for (d = 0; d < 2; d++) {
            dir = d ? 1 : -1;
            nx = (uint8_t)(x + dir);
            /* walk (and drop off a ledge) */
            if (!solid(nx, y, g)) {
                try_land(nx, y, g);
            } else if (!solid(nx, (uint8_t)(y + up), g) && !solid(x, (uint8_t)(y + up), g) &&
                       !deadly(nx, (uint8_t)(y + up))) {
                /* climb a one-cell step */
                try_land(nx, (uint8_t)(y + up), g);
            }
            /* jump a gap at this height: one cell needs one cell of headroom, two cells
             * need two (a lower ceiling cuts the jump short) */
            if (!solid(x, (uint8_t)(y + up), g)) {
                uint8_t room2 = (uint8_t)!solid(x, (uint8_t)(y + 2 * up), g);
                for (k = 1; k <= 3; k++) {
                    uint8_t xx = (uint8_t)(x + dir * k);
                    if (xx >= st->w || solid(xx, y, g) || solid(xx, (uint8_t)(y + up), g) ||
                        deadly(xx, (uint8_t)(y + up)))
                        break;
                    if (k == 3) {
                        if (!room2) break;
                        if (solid((uint8_t)(x + dir), (uint8_t)(y + 2 * up), g) ||
                            solid((uint8_t)(x + 2 * dir), (uint8_t)(y + 2 * up), g))
                            break;
                    }
                    if (k >= 2 && !deadly(xx, y)) try_land(xx, y, g);
                }
            }
        }
        /* flip: fall straight across to the other side, or drift one column over (the
         * cell beside the mech must be clear to start the drift) */
        ng = (uint8_t)(g ^ 1);
        {
            uint8_t t = C->cell[SCELL(x, y)];
            if (!((t == T_PHASE_P && ng == G_DOWN) || (t == T_PHASE_G && ng == G_UP))) {
                r = fall(x, y, ng);
                if (r != 0xFF) visit(x, r, ng);
                for (d = 0; d < 2; d++) {
                    nx = (uint8_t)(x + (d ? 1 : -1));
                    if (nx < st->w && !solid(nx, y, ng) && !solid(nx, y, g)) try_land(nx, y, ng);
                }
            }
        }
    }
    return 0;
}
