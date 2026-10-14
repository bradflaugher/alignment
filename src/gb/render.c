/* ALIGNMENT - turning a stage into background tiles.
 * The BG map is a 16-cell ring: stage column cx lives in map tile columns
 * (cx * 2) & 31 and +1, all 32 tile rows (the stage is exactly 16 cells tall). */
#pragma bank 255
#include <gb/gb.h>
#include <string.h>
#include "gfx.h"
#include "render.h"
#include "assets.h"

extern uint8_t r_wall_tile[];
extern uint8_t r_mt_tiles[];
extern uint8_t r_mt_pal[];

static const Stage *cS;
static uint8_t q_t[4], q_a[4];
static uint8_t col_t[2][32], col_a[2][32];
uint8_t render_checked;     /* the checkpoint is lit */

/* wall flags of the column being built and its two neighbours, with a wall row of
 * padding above and below: wl / wc / wr [y + 1] */
static uint8_t wl[SH + 2], wc[SH + 2], wr[SH + 2];

static void wall_flags(uint8_t *dst, int16_t x)
{
    uint8_t y;
    const uint8_t *src;
    dst[0] = dst[SH + 1] = 1;
    if (x < 0 || x >= cS->w) {
        for (y = 1; y <= SH; y++) dst[y] = 1;
        return;
    }
    src = &cS->cell[SCELL(x, 0)];
    for (y = 0; y < SH; y++) dst[y + 1] = (uint8_t)(src[y] == T_WALL);
}

/* a cheap per-cell hash: rows add 13, columns 7 (no multiply, no divide) */
static uint8_t col_hash;
#define HASH(y) ((uint8_t)(col_hash + (uint8_t)((y) * 13)))

/* wall variant from (vertical neighbour, horizontal neighbour, diagonal) as v | h<<1 | d<<2 */
static const uint8_t pick_tab[8] = {
    WV_OUTER, WV_VEDGE, WV_HEDGE, WV_INNER, WV_OUTER, WV_VEDGE, WV_HEDGE, WV_FILL
};

static void wall_quarters(uint8_t y, uint8_t h)
{
    uint8_t r = (uint8_t)(y + 1), v;
    uint8_t f2 = (uint8_t)(((h ^ (h >> 3)) & 7) == 0);    /* about one cell in eight */
    v = pick_tab[wc[r - 1] | (wl[r] << 1) | (wl[r - 1] << 2)];
    if (f2 && v == WV_FILL) v = WV_FILL2;
    q_t[0] = r_wall_tile[0 * WALL_VARIANTS + v];
    v = pick_tab[wc[r - 1] | (wr[r] << 1) | (wr[r - 1] << 2)];
    if (f2 && v == WV_FILL) v = WV_FILL2;
    q_t[1] = r_wall_tile[1 * WALL_VARIANTS + v];
    v = pick_tab[wc[r + 1] | (wl[r] << 1) | (wl[r + 1] << 2)];
    if (f2 && v == WV_FILL) v = WV_FILL2;
    q_t[2] = r_wall_tile[2 * WALL_VARIANTS + v];
    v = pick_tab[wc[r + 1] | (wr[r] << 1) | (wr[r + 1] << 2)];
    if (f2 && v == WV_FILL) v = WV_FILL2;
    q_t[3] = r_wall_tile[3 * WALL_VARIANTS + v];
    q_a[0] = q_a[1] = q_a[2] = q_a[3] = PAL_WALL;
}

static uint8_t cell_mt(uint8_t t, uint8_t y, uint8_t h)
{
    switch (t) {
    case T_PLASMA: return MT_PLASMA;
    case T_PHASE_P: return MT_PHASE_P_ON;     /* (their art follows gravity: gfx_phase) */
    case T_PHASE_G: return MT_PHASE_G_ON;
    case T_CELL: return MT_CELL;
    case T_CHECK: return render_checked ? MT_PLATE_F_ON : MT_PLATE_F;
    case T_EXIT: return MT_EXIT_R;
    case T_LASER: return MT_DOOR_CLOSED;      /* (blinks: gfx_laser) */
    case T_GIRDER: return MT_BACK1;
    case T_ITEM_P: return MT_ITEM_POWER;
    case T_ITEM_S: return MT_ITEM_SHIELD;
    case T_ITEM_B: return MT_ITEM_BOMB;
    case T_ITEM_R: return MT_ITEM_REPAIR;
    case T_DOOR: return MT_BOSS_DOOR;
    case T_DOOR_OPEN: return MT_EMPTY;
    case T_DECO_PIPE: return MT_DECO_PIPE;
    case T_DECO_PIPE_V: return MT_DECO_PIPE_V;
    case T_DECO_WINDOW: return MT_DECO_WINDOW;
    case T_DECO_LIGHT: return MT_DECO_LIGHT;
    case T_DECO_GRATE: return MT_DECO_GRATE;
    }
    /* empty: now and then a little background machinery, never right by a floor (the
     * hand-made sectors place their own) */
    if (cS->sector) return MT_EMPTY;
    if ((h & 15) == 3 && !wc[y + 2] && !wc[y]) return MT_BACK2;
    if ((h & 31) == 9) return MT_BACK3;
    return MT_EMPTY;
}

static void cell_quarters(const uint8_t *col, uint8_t y)
{
    uint8_t m, t = col[y], h = HASH(y), p;
    const uint8_t *mt;
    if (t == T_WALL) {
        wall_quarters(y, h);
        return;
    }
    m = cell_mt(t, y, h);
    mt = &r_mt_tiles[m << 2];
    p = r_mt_pal[m];
    q_t[0] = mt[0];
    q_t[1] = mt[1];
    q_t[2] = mt[2];
    q_t[3] = mt[3];
    q_a[0] = q_a[1] = q_a[2] = q_a[3] = p;
}

static void flags_for(uint8_t cx)
{
    col_hash = (uint8_t)(cx * 7 + (uint8_t)cS->index * 5);
    wall_flags(wl, (int16_t)cx - 1);
    wall_flags(wc, cx);
    wall_flags(wr, (int16_t)cx + 1);
}

static void build_column(const Stage *S, uint8_t cx)
{
    uint8_t y, r;
    const uint8_t *col = &S->cell[SCELL(cx, 0)];
    cS = S;
    flags_for(cx);
    for (y = 0; y < SH; y++) {
        cell_quarters(col, y);
        r = (uint8_t)(y * 2);
        col_t[0][r] = q_t[0];
        col_t[1][r] = q_t[1];
        col_t[0][r + 1] = q_t[2];
        col_t[1][r + 1] = q_t[3];
        col_a[0][r] = q_a[0];
        col_a[1][r] = q_a[1];
        col_a[0][r + 1] = q_a[2];
        col_a[1][r + 1] = q_a[3];
    }
}

static void store_column(uint8_t cx)
{
    uint8_t *dst = (uint8_t *)(0xA000u + ((uint16_t)cx << 6));
    ENABLE_RAM;
    SWITCH_RAM(2);
    memcpy(dst, col_t[0], 32);
    memcpy(dst + 32, col_t[1], 32);
    SWITCH_RAM(3);
    memcpy(dst, col_a[0], 32);
    memcpy(dst + 32, col_a[1], 32);
}

void render_prebuild(const Stage *S) BANKED
{
    uint8_t cx;
    stream_on = 0;
    for (cx = 0; cx < S->w; cx++) {
        build_column(S, cx);
        store_column(cx);
    }
}

void render_column(const Stage *S, uint8_t cx) BANKED
{
    (void)S;
    col_push(cx);
}

void render_rebuild(const Stage *S, uint8_t cx, uint8_t on_map) BANKED
{
    uint8_t on = stream_on;
    stream_on = 0;
    build_column(S, cx);
    store_column(cx);
    stream_on = on;
    if (on_map) col_push(cx);
}

void render_cell(const Stage *S, uint8_t cx, uint8_t cy) BANKED
{
    /* only cells that turn into something simple change during play: an energy cell
     * taken (empty) or the checkpoint lit; patch its 4 tiles in cartridge RAM and on the
     * map, no column rebuild */
    uint8_t x = (uint8_t)((cx * 2) & 31), y = (uint8_t)(cy * 2), on = stream_on, m, p;
    uint8_t t = S->cell[SCELL(cx, cy)];
    uint8_t *dst;
    const uint8_t *mt;
    m = t == T_CHECK ? (render_checked ? MT_PLATE_F_ON : MT_PLATE_F) : MT_EMPTY;
    mt = &r_mt_tiles[m << 2];
    p = r_mt_pal[m];
    stream_on = 0;
    dst = (uint8_t *)(0xA000u + ((uint16_t)cx << 6) + y);
    ENABLE_RAM;
    SWITCH_RAM(2);
    dst[0] = mt[0];
    dst[32] = mt[1];
    dst[1] = mt[2];
    dst[33] = mt[3];
    SWITCH_RAM(3);
    dst[0] = dst[32] = dst[1] = dst[33] = p;
    stream_on = on;
    q_t[0] = mt[0];
    q_t[1] = mt[1];
    q_t[2] = mt[2];
    q_t[3] = mt[3];
    q_a[0] = q_a[1] = q_a[2] = q_a[3] = p;
    put_block(x, y, 2, 1, &q_t[0], &q_a[0]);
    put_block(x, (uint8_t)(y + 1), 2, 1, &q_t[2], &q_a[2]);
}
