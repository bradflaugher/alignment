/* ALIGNMENT - room rendering: auto-tiled hull, metatiles, the dynamic cells. */
#pragma bank 255
#include <gb/gb.h>
#include <string.h>
#include "gfx.h"
#include "render.h"

uint8_t room_ox, room_oy;
uint8_t render_hide;
uint8_t rbuf_t[ROOM_TW * ROOM_TH];
uint8_t rbuf_a[ROOM_TW * ROOM_TH];

static const Room *cR;
static const State *cS;
static uint8_t q_t[4], q_a[4];

static uint8_t is_wall(int8_t x, int8_t y)
{
    if (x < 0 || y < 0 || x >= RW || y >= RH) return 1;
    return (uint8_t)(cR->cell[CELL(x, y)] == T_WALL);
}

static uint8_t hash_xy(uint8_t x, uint8_t y)
{
    return (uint8_t)(x * 7 + y * 13 + x * y + (uint8_t)cR->index * 5);
}

static void wall_quarters(uint8_t x, uint8_t y)
{
    /* each 8x8 quarter looks at its two orthogonal neighbours and the diagonal */
    static const int8_t qdx[4] = { -1, 1, -1, 1 };
    static const int8_t qdy[4] = { -1, -1, 1, 1 };
    uint8_t q, v, h, d, var, fill2;
    fill2 = (uint8_t)((hash_xy(x, y) % 6) == 0);
    for (q = 0; q < 4; q++) {
        v = is_wall((int8_t)x, (int8_t)(y + qdy[q]));
        h = is_wall((int8_t)(x + qdx[q]), (int8_t)y);
        if (v && h) {
            d = is_wall((int8_t)(x + qdx[q]), (int8_t)(y + qdy[q]));
            var = d ? (fill2 ? WV_FILL2 : WV_FILL) : WV_INNER;
        } else if (v) {
            var = WV_VEDGE;
        } else if (h) {
            var = WV_HEDGE;
        } else {
            var = WV_OUTER;
        }
        q_t[q] = r_wall_tile[q * WALL_VARIANTS + var];
        q_a[q] = PAL_WALL;
    }
}

static uint8_t crate_at(uint8_t c)
{
    uint8_t i;
    for (i = 0; i < MAX_CRATES; i++)
        if (cS->o[SLOT_CRATE + i] == c && !(render_hide & (1u << i))) return 1;
    return 0;
}

static uint8_t occupied(uint8_t c)
{
    uint8_t i;
    for (i = 0; i < NOBJ; i++)
        if (cS->o[i] == c) return 1;
    return 0;
}

static uint8_t cell_mt(uint8_t c)
{
    uint8_t t = cR->cell[c], x, y, h;
    if (crate_at(c)) return MT_CRATE;
    switch (t) {
    case T_PLASMA: return MT_PLASMA;
    case T_PHASE_P: return cS->g == G_DOWN ? MT_PHASE_P_ON : MT_PHASE_P_OFF;
    case T_PHASE_G: return cS->g == G_UP ? MT_PHASE_G_ON : MT_PHASE_G_OFF;
    case T_PLATE:
        if (cR->cell[c + RW] == T_WALL || !(cR->cell[c - RW] == T_WALL))
            return occupied(c) ? MT_PLATE_F_ON : MT_PLATE_F;
        return occupied(c) ? MT_PLATE_C_ON : MT_PLATE_C;
    case T_DOOR: return doors_open(cR, cS) ? MT_DOOR_OPEN : MT_DOOR_CLOSED;
    case T_PICK0: case T_PICK1: case T_PICK2:
        if (cS->got & (1u << (t - T_PICK0))) break;
        return MT_CELL;
    case T_ENTRY:
        return cR->entry_side == SIDE_L ? MT_HATCH_L : cR->entry_side == SIDE_T ? MT_HATCH_T : MT_HATCH_B;
    case T_EXIT:
        if (cS->got == room_full(cR))
            return cR->exit_side == SIDE_R ? MT_EXIT_R : cR->exit_side == SIDE_T ? MT_EXIT_U : MT_EXIT_D;
        return cR->exit_side == SIDE_R ? MT_FIELD_V : MT_FIELD_H;
    }
    /* empty: now and then a little background machinery */
    x = CX(c);
    y = CY(c);
    h = hash_xy(x, y);
    if ((h & 15) == 3) return MT_BACK1;
    if ((h & 15) == 9) return MT_BACK2;
    if ((h & 31) == 14) return MT_BACK3;
    return MT_EMPTY;
}

static void cell_quarters(uint8_t c)
{
    uint8_t m, i;
    if (cR->cell[c] == T_WALL) {
        wall_quarters(CX(c), CY(c));
        return;
    }
    m = cell_mt(c);
    for (i = 0; i < 4; i++) {
        q_t[i] = r_mt_tiles[m * 4 + i];
        q_a[i] = r_mt_pal[m];
    }
}

void render_build(const Room *R, const State *s) BANKED
{
    uint8_t c, i, x, y;
    cR = R;
    cS = s;
    for (c = 0; c < RN; c++) {
        cell_quarters(c);
        x = (uint8_t)(CX(c) * 2);
        y = (uint8_t)(CY(c) * 2);
        i = (uint8_t)(y * ROOM_TW + x);
        rbuf_t[i] = q_t[0];
        rbuf_t[i + 1] = q_t[1];
        rbuf_t[i + ROOM_TW] = q_t[2];
        rbuf_t[i + ROOM_TW + 1] = q_t[3];
        rbuf_a[i] = q_a[0];
        rbuf_a[i + 1] = q_a[1];
        rbuf_a[i + ROOM_TW] = q_a[2];
        rbuf_a[i + ROOM_TW + 1] = q_a[3];
    }
}

void render_blit_all(uint8_t ox, uint8_t oy) BANKED
{
    uint8_t r;
    for (r = 0; r < ROOM_TH; r++) render_blit_row(r, ox, oy);
}

void render_blit_row(uint8_t row, uint8_t ox, uint8_t oy) BANKED
{
    uint8_t y = (uint8_t)((oy + row) & 31);
    uint8_t first = (uint8_t)(32 - (ox & 31));
    const uint8_t *t = rbuf_t + row * ROOM_TW, *a = rbuf_a + row * ROOM_TW;
    ox &= 31;
    if (first >= ROOM_TW) {
        put_block(ox, y, ROOM_TW, 1, t, a);
    } else {
        put_block(ox, y, first, 1, t, a);
        put_block(0, y, (uint8_t)(ROOM_TW - first), 1, t + first, a + first);
    }
}

void render_blit_col(uint8_t col, uint8_t ox, uint8_t oy) BANKED
{
    uint8_t t[ROOM_TH], a[ROOM_TH], r, x = (uint8_t)((ox + col) & 31);
    uint8_t first = (uint8_t)(32 - (oy & 31));
    for (r = 0; r < ROOM_TH; r++) {
        t[r] = rbuf_t[r * ROOM_TW + col];
        a[r] = rbuf_a[r * ROOM_TW + col];
    }
    oy &= 31;
    if (first >= ROOM_TH) {
        put_block(x, oy, 1, ROOM_TH, t, a);
    } else {
        put_block(x, oy, 1, first, t, a);
        put_block(x, 0, 1, (uint8_t)(ROOM_TH - first), t + first, a + first);
    }
}

void render_cell(const Room *R, const State *s, uint8_t c) BANKED
{
    uint8_t x, y, tl[2], al[2];
    cR = R;
    cS = s;
    cell_quarters(c);
    x = (uint8_t)((room_ox + CX(c) * 2) & 31);
    y = (uint8_t)((room_oy + CY(c) * 2) & 31);
    tl[0] = q_t[0]; tl[1] = q_t[1];
    al[0] = q_a[0]; al[1] = q_a[1];
    if (x == 31) {
        put_block(31, y, 1, 1, &tl[0], &al[0]);
        put_block(0, y, 1, 1, &tl[1], &al[1]);
    } else {
        put_block(x, y, 2, 1, tl, al);
    }
    y = (uint8_t)((y + 1) & 31);
    tl[0] = q_t[2]; tl[1] = q_t[3];
    al[0] = q_a[2]; al[1] = q_a[3];
    if (x == 31) {
        put_block(31, y, 1, 1, &tl[0], &al[0]);
        put_block(0, y, 1, 1, &tl[1], &al[1]);
    } else {
        put_block(x, y, 2, 1, tl, al);
    }
}

void render_dynamic(const Room *R, const State *s) BANKED
{
    uint8_t c, t;
    for (c = 0; c < RN; c++) {
        t = R->cell[c];
        if (t == T_PHASE_P || t == T_PHASE_G || t == T_DOOR || t == T_PLATE || t == T_EXIT)
            render_cell(R, s, c);
    }
}
