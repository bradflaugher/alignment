/* ALIGNMENT - physics. Bank 0 on the Game Boy (it runs every frame).
 *
 * The mech runs, jumps (B) and flips gravity (A): on the ground any time, in the air
 * once until it lands again. A flip is refused while the mech is inside a phase block
 * that the new gravity would make solid. Collisions are against 16x16 cells; the
 * hitbox is smaller than a cell, so testing its corners is enough, and no speed is ever
 * more than a few pixels a frame, so nothing tunnels. Banked: it only reads RAM. */
#ifdef __SDCC
#pragma bank 255
#endif
#include "phys.h"

/* gravity-relative helpers without multiplies (SDCC would call a 16-bit multiply) */
#define ALONG(v) (gd > 0 ? (int16_t)(v) : (int16_t)-(v))       /* v * gd */

uint8_t ev_cx, ev_cy, ev_item;

void mech_spawn(Mech *m, uint8_t cx, uint8_t cy, uint8_t g) CORE_BANKED
{
    m->x = (uint16_t)((uint16_t)cx * 16 * SUB);
    m->y = (uint16_t)((uint16_t)cy * 16 * SUB);
    m->vx = 0;
    m->vy = 0;
    m->g = g;
    m->ground = 0;
    m->airflip = 1;
    m->facing = 0;
    m->coyote = 0;
    m->dead = DIE_NONE;
    m->land_v = 0;
    m->step_t = 0;
}

static const Stage *cs;
static const uint8_t *cells;
static uint8_t cw, cg;

/* the cell under pixel (px, py): column-major, 16 bytes a column */
#define cell_t(px, py) ((uint8_t)((uint16_t)(px) >> 4) >= cw ? (uint8_t)T_WALL : \
                        cells[((uint16_t)(px) & 0xFFF0u) | (uint8_t)((uint8_t)(py) >> 4)])

uint8_t solid_px(const Stage *S, uint16_t px, uint8_t py, uint8_t g) CORE_BANKED
{
    uint8_t t;
    cs = S;
    cells = S->cell;
    cw = S->w;
    t = cell_t(px, py);
    return (uint8_t)(t == T_WALL || t == T_DOOR || (t == T_PHASE_P && g == G_DOWN) || (t == T_PHASE_G && g == G_UP));
}

/* solid for the mech in each gravity, by terrain (a table: this runs ~20 times a frame) */
static const uint8_t solid_tab[2][NUM_T] = {
    /* down */ { 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
    /* up   */ { 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
};
typedef char solid_tab_covers_terrain[NUM_T == 21 ? 1 : -1];
static const uint8_t *solid_g;      /* solid_tab[the gravity of this step] */
#define SOLID_C(px, py) (solid_g[cell_t((px), (py))])

/* does the hitbox with its top-left at pixel (hx, hy) touch anything solid? */
static uint8_t box_hits(uint16_t hx, uint8_t hy)
{
    uint16_t rx = (uint16_t)(hx + HB_W - 1);
    uint8_t by = (uint8_t)(hy + HB_H - 1);
    return (uint8_t)(SOLID_C(hx, hy) || SOLID_C(rx, hy) || SOLID_C(hx, by) || SOLID_C(rx, by));
}

/* does the box touch a cell of type t? */
static uint8_t box_type(uint16_t hx, uint8_t hy, uint8_t w, uint8_t h, uint8_t t)
{
    uint16_t rx = (uint16_t)(hx + w - 1);
    uint8_t by = (uint8_t)(hy + h - 1);
    if (cell_t(hx, hy) == t) { ev_cx = (uint8_t)(hx >> 4); ev_cy = (uint8_t)(hy >> 4); return 1; }
    if (cell_t(rx, hy) == t) { ev_cx = (uint8_t)(rx >> 4); ev_cy = (uint8_t)(hy >> 4); return 1; }
    if (cell_t(hx, by) == t) { ev_cx = (uint8_t)(hx >> 4); ev_cy = (uint8_t)(by >> 4); return 1; }
    if (cell_t(rx, by) == t) { ev_cx = (uint8_t)(rx >> 4); ev_cy = (uint8_t)(by >> 4); return 1; }
    return 0;
}

uint8_t laser_on(uint16_t t) CORE_BANKED
{
    return (uint8_t)(((t >> 3) & 15) < 9);        /* on 72 frames, off 56 */
}

#include "mul8.h"

/* a triangle wave on an 8-bit phase: 0..len..0 every 256 steps (no division) */
static uint16_t tri(uint8_t ph, uint8_t len)
{
    uint8_t t = ph < 128 ? ph : (uint8_t)(255 - ph);
    return (uint16_t)(mul8(t, len) >> 7);
}

void enemy_pos(const Enemy *e, uint16_t t, uint16_t *px, uint8_t *py, uint8_t *len) CORE_BANKED
{
    uint16_t tt = (uint16_t)(t + ((uint16_t)e->phase << 2));
    uint8_t ct, ph, range;
    /* tt * speed (1..3) */
    if (e->speed == 2) tt <<= 1;
    else if (e->speed == 3) tt = (uint16_t)(tt + (tt << 1));
    ph = (uint8_t)(tt >> 1);
    *len = 0;
    *px = (uint16_t)e->x0 << 4;
    *py = (uint8_t)(e->y0 << 4);
    switch (e->kind) {
    case E_CRAWLER:
        *px += tri(ph, (uint8_t)((e->x1 - e->x0) << 4));
        break;
    case E_HOVER:
        *py = (uint8_t)(*py + tri(ph, (uint8_t)((e->y1 - e->y0) << 4)));
        break;
    case E_TURRET:
        if (e->speed) *px = (uint16_t)e->x1 << 4;
        break;
    default: /* E_CRUSHER: rest, slam, hold, pull back up */
        range = (uint8_t)((e->y1 - e->y0) << 4);
        ct = (uint8_t)(tt & 127);
        if (ct < 60) *len = 0;
        else if (ct < 68) *len = (uint8_t)(mul8(range, (uint8_t)(ct - 60)) >> 3);
        else if (ct < 92) *len = range;
        else {
            uint16_t v = mul8(range, (uint8_t)(127 - ct));
            *len = (uint8_t)((uint16_t)((v << 3) - v) >> 8);
        }
        break;
    }
}

uint8_t turret_bolt(const Enemy *e, uint16_t t, uint16_t *bx) CORE_BANKED
{
    /* a bolt every 128 frames, 2 px a frame, after 8 frames of charging */
    uint8_t age = (uint8_t)((uint8_t)(t + (uint16_t)(e->phase << 2)) & 127);
    uint16_t x, travel;
    if (!e->hp || age < 8) return 0;
    travel = (uint16_t)((uint8_t)(age - 8) << 1);
    if (e->speed) {
        x = (uint16_t)(((uint16_t)e->x1 << 4) - 4);
        if (travel > x || (uint16_t)(x - travel) < ((uint16_t)e->x0 << 4)) return 0;
        *bx = (uint16_t)(x - travel);
    } else {
        x = (uint16_t)(((uint16_t)e->x0 << 4) + 12);
        *bx = (uint16_t)(x + travel);
        if (*bx + 8 > ((uint16_t)(e->x1 + 1) << 4)) return 0;
    }
    return 1;
}


uint16_t mech_step(Mech *m, Stage *S, uint8_t in, uint16_t t) CORE_BANKED
{
    uint16_t ev = 0, px, hx, nx;
    uint8_t py, hy, ny, ey, was_ground = m->ground;
    int8_t gd;
    int16_t target, v;

    cs = S;
    cells = S->cell;
    cw = S->w;
    cg = m->g;
    solid_g = solid_tab[cg];
    gd = m->g ? -1 : 1;

    /* ---- run ---- */
    target = (in & IN_L) ? -RUN_MAX : (in & IN_R) ? RUN_MAX : 0;
    if (in & IN_L) m->facing = 1;
    else if (in & IN_R) m->facing = 0;
    if (target) {
        v = m->ground ? RUN_ACC : AIR_ACC;
        if (m->vx < target) { m->vx += v; if (m->vx > target) m->vx = target; }
        else if (m->vx > target) { m->vx -= v; if (m->vx < target) m->vx = target; }
    } else {
        v = m->ground ? RUN_DEC : 1;
        if (m->vx > 0) { m->vx -= v; if (m->vx < 0) m->vx = 0; }
        else if (m->vx < 0) { m->vx += v; if (m->vx > 0) m->vx = 0; }
    }

    /* ---- jump ---- */
    if ((in & IN_JUMP_NEW) && (m->ground || m->coyote)) {
        m->vy = ALONG(-JUMP_V);
        m->ground = 0;
        m->coyote = 0;
        ev |= EV_JUMP;
    }
    /* let go early for a short hop */
    if (!(in & IN_JUMP) && ALONG(m->vy) < -(JUMP_V / 3))
        m->vy = ALONG(-(JUMP_V / 3));

    /* ---- flip ---- */
    if (in & IN_FLIP_NEW) {
        px = (uint16_t)(m->x >> 4);
        py = (uint8_t)(m->y >> 4);
        hx = (uint16_t)(px + HB_X);
        hy = (uint8_t)(py + HB_Y);
        /* the blocks the new gravity makes solid can't form around the mech */
        if ((!m->ground && !m->airflip) ||
            box_type(hx, hy, HB_W, HB_H, m->g ? T_PHASE_P : T_PHASE_G)) {
            ev |= EV_NOFLIP;
        } else {
            if (!m->ground) m->airflip = 0;
            m->g ^= 1;
            cg = m->g;
            solid_g = solid_tab[cg];
            gd = (int8_t)-gd;
            m->ground = 0;
            m->coyote = 0;
            m->vy = ALONG(FLIP_KICK);
            ev |= EV_FLIP;
        }
    }

    /* ---- gravity ---- */
    m->vy += ALONG(GRAV);
    if (m->vy > FALL_MAX) m->vy = FALL_MAX;
    if (m->vy < -FALL_MAX) m->vy = -FALL_MAX;

    /* ---- move: x, then y ---- */
    nx = (uint16_t)(m->x + m->vx);
    px = (uint16_t)(nx >> 4);
    py = (uint8_t)(m->y >> 4);
    if (box_hits((uint16_t)(px + HB_X), (uint8_t)(py + HB_Y))) {
        if (m->vx > 0) {
            hx = (uint16_t)((((px + HB_X + HB_W - 1) >> 4) << 4) - HB_X - HB_W);
        } else {
            hx = (uint16_t)(((((px + HB_X) >> 4) + 1) << 4) - HB_X);
        }
        m->x = (uint16_t)(hx << 4);
        m->vx = 0;
    } else {
        m->x = nx;
    }
    px = (uint16_t)(m->x >> 4);
    py = (uint8_t)(m->y >> 4);
    /* standing: if the ground is still there, stay on it (no sub-pixel creeping) */
    if (m->ground && ALONG(m->vy) >= 0) {
        ey = gd > 0 ? (uint8_t)(py + HB_Y + HB_H) : (uint8_t)(py + HB_Y - 1);
        if (SOLID_C((uint16_t)(px + HB_X), ey) || SOLID_C((uint16_t)(px + HB_X + HB_W - 1), ey)) {
            m->vy = 0;
            m->y = (uint16_t)((uint16_t)py << 4);
            goto moved;
        }
    }
    ny = (uint8_t)((uint16_t)(m->y + m->vy) >> 4);
    if (box_hits((uint16_t)(px + HB_X), (uint8_t)(ny + HB_Y))) {
        if (m->vy > 0) {
            hy = (uint8_t)((((ny + HB_Y + HB_H - 1) >> 4) << 4) - HB_Y - HB_H);
        } else {
            hy = (uint8_t)(((((ny + HB_Y) >> 4) + 1) << 4) - HB_Y);
        }
        m->y = (uint16_t)((uint16_t)hy << 4);
        if (ALONG(m->vy) > 0) {
            /* landed */
            if (!was_ground) {
                ev |= EV_LAND;
                m->land_v = (uint8_t)((m->vy < 0 ? -m->vy : m->vy) >> 2);
            }
            m->ground = 1;
            m->airflip = 1;
        } else {
            ev |= EV_BONK;
        }
        m->vy = 0;
    } else {
        m->y = (uint16_t)(m->y + m->vy);
        if (m->ground) m->coyote = COYOTE;
        m->ground = 0;
        if (m->coyote) m->coyote--;
    }
moved:
    px = (uint16_t)(m->x >> 4);
    py = (uint8_t)(m->y >> 4);
    /* still standing? (walking off a ledge) */
    if (m->ground) {
        ey = gd > 0 ? (uint8_t)(py + HB_Y + HB_H) : (uint8_t)(py + HB_Y - 1);
        if (!SOLID_C((uint16_t)(px + HB_X), ey) && !SOLID_C((uint16_t)(px + HB_X + HB_W - 1), ey)) {
            m->ground = 0;
            m->coyote = COYOTE;
        } else if (m->vx) {
            m->step_t += (uint8_t)(m->vx < 0 ? -m->vx : m->vx);
            if (m->step_t >= 160) {
                m->step_t = 0;
                ev |= EV_STEP;
            }
        }
    }

    /* ---- things it touches: each corner's cell is read once ---- */
    hx = (uint16_t)(px + HB_X);
    hy = (uint8_t)(py + HB_Y);
    {
        /* hazards get a slightly smaller box: brushing past is fine */
        uint8_t a = cell_t((uint16_t)(hx + 1), (uint8_t)(hy + 2));
        uint8_t b = cell_t((uint16_t)(hx + HB_W - 2), (uint8_t)(hy + 2));
        uint8_t c = cell_t((uint16_t)(hx + 1), (uint8_t)(hy + HB_H - 3));
        uint8_t d = cell_t((uint16_t)(hx + HB_W - 2), (uint8_t)(hy + HB_H - 3));
        if (a == T_PLASMA || b == T_PLASMA || c == T_PLASMA || d == T_PLASMA) m->dead = DIE_PLASMA;
        else if ((a == T_LASER || b == T_LASER || c == T_LASER || d == T_LASER) && laser_on(t)) m->dead = DIE_LASER;
    }
    if (!m->dead) {
        /* pickups and gates under the hitbox's corners (one pickup a frame) */
        uint8_t k, t, cx, cy;
        for (k = 0; k < 4; k++) {
            cx = (uint8_t)(((k & 1) ? (uint16_t)(hx + HB_W - 1) : hx) >> 4);
            cy = (uint8_t)(((k & 2) ? (uint8_t)(hy + HB_H - 1) : hy) >> 4);
            if (cx >= cw) continue;
            t = cells[SCELL(cx, cy)];
            if (t < T_CELL) continue;
            if (t == T_CELL || T_IS_ITEM(t)) {
                S->cell[SCELL(cx, cy)] = T_EMPTY;
                ev_cx = cx;
                ev_cy = cy;
                if (t == T_CELL) {
                    ev |= EV_CELL;
                } else {
                    ev_item = t;
                    ev |= EV_ITEM;
                }
                break;
            }
            if (t == T_CHECK) ev |= EV_CHECK;
            else if (t == T_EXIT) ev |= EV_EXIT;
        }
    }
    if (m->dead) return (uint16_t)((ev & ~EV_EXIT) | EV_DEAD);
    return ev;
}
