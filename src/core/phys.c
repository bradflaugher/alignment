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

uint8_t ev_cx, ev_cy;

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
static uint8_t cell_t(uint16_t px, uint8_t py)
{
    uint8_t cx = (uint8_t)(px >> 4);
    if (cx >= cw) return T_WALL;
    return cells[(uint16_t)(px & 0xFFF0u) | (py >> 4)];
}

uint8_t solid_px(const Stage *S, uint16_t px, uint8_t py, uint8_t g) CORE_BANKED
{
    uint8_t t;
    cs = S;
    cells = S->cell;
    cw = S->w;
    t = cell_t(px, py);
    return (uint8_t)(t == T_WALL || (t == T_PHASE_P && g == G_DOWN) || (t == T_PHASE_G && g == G_UP));
}

static uint8_t solid_c(uint16_t px, uint8_t py)
{
    uint8_t t = cell_t(px, py);
    return (uint8_t)(t == T_WALL || (t == T_PHASE_P && cg == G_DOWN) || (t == T_PHASE_G && cg == G_UP));
}

/* does the hitbox with its top-left at pixel (hx, hy) touch anything solid? */
static uint8_t box_hits(uint16_t hx, uint8_t hy)
{
    uint16_t rx = (uint16_t)(hx + HB_W - 1);
    uint8_t by = (uint8_t)(hy + HB_H - 1);
    return (uint8_t)(solid_c(hx, hy) || solid_c(rx, hy) || solid_c(hx, by) || solid_c(rx, by));
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

/* a triangle wave on an 8-bit phase: 0..len..0 every 256 steps (no division) */
static uint16_t tri(uint8_t ph, uint16_t len)
{
    uint8_t t = ph < 128 ? ph : (uint8_t)(255 - ph);
    return (uint16_t)((t * len) >> 7);
}

void enemy_pos(const Enemy *e, uint16_t t, uint16_t *px, uint8_t *py, uint8_t *len) CORE_BANKED
{
    uint16_t tt = (uint16_t)(t + (uint16_t)e->phase * 4);
    uint8_t ct, ph = (uint8_t)((uint16_t)(tt * e->speed) >> 1);
    uint16_t range;
    *len = 0;
    switch (e->kind) {
    case E_CRAWLER:
        *px = (uint16_t)(e->x0 * 16 + tri(ph, (uint16_t)((e->x1 - e->x0) * 16)));
        *py = (uint8_t)(e->y0 * 16);
        break;
    case E_HOVER:
        *px = (uint16_t)(e->x0 * 16);
        *py = (uint8_t)(e->y0 * 16 + tri(ph, (uint16_t)((e->y1 - e->y0) * 16)));
        break;
    default: /* E_CRUSHER: rest, slam, hold, pull back up */
        *px = (uint16_t)(e->x0 * 16);
        *py = (uint8_t)(e->y0 * 16);
        range = (uint16_t)((e->y1 - e->y0) * 16);
        ct = (uint8_t)((tt * e->speed) & 127);
        if (ct < 60) *len = 0;
        else if (ct < 68) *len = (uint8_t)((range * (uint8_t)(ct - 60)) >> 3);
        else if (ct < 92) *len = (uint8_t)range;
        else *len = (uint8_t)((range * (uint8_t)(127 - ct) * 7) >> 8);
        break;
    }
}

static uint8_t overlap(uint16_t ax, uint8_t ay, uint8_t aw, uint8_t ah,
                       uint16_t bx, uint8_t by, uint8_t bw, uint8_t bh)
{
    return (uint8_t)(ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah);
}

uint16_t mech_step(Mech *m, Stage *S, uint8_t in, uint16_t t) CORE_BANKED
{
    uint16_t ev = 0, px, hx, nx;
    uint8_t py, hy, ny, i, len, ey, was_ground = m->ground;
    int8_t gd;
    int16_t target, v;

    cs = S;
    cells = S->cell;
    cw = S->w;
    cg = m->g;
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
        if (solid_c((uint16_t)(px + HB_X), ey) || solid_c((uint16_t)(px + HB_X + HB_W - 1), ey)) {
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
        if (!solid_c((uint16_t)(px + HB_X), ey) && !solid_c((uint16_t)(px + HB_X + HB_W - 1), ey)) {
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
        /* the rest only matters if one of them is something to touch */
        if ((uint8_t)(a | b | c | d) >= T_CELL && !m->dead) {
            if (box_type(hx, hy, HB_W, HB_H, T_CELL)) {
                S->cell[SCELL(ev_cx, ev_cy)] = T_EMPTY;
                ev |= EV_CELL;
            }
            if (box_type(hx, hy, HB_W, HB_H, T_CHECK)) ev |= EV_CHECK;
            if (box_type(hx, hy, HB_W, HB_H, T_EXIT)) ev |= EV_EXIT;
        }
    }
    {
        uint8_t mcx = (uint8_t)(px >> 4);
    for (i = 0; i < S->nenemies && !m->dead; i++) {
        const Enemy *e = &S->enemy[i];
        uint16_t ex;
        uint8_t eyy;
        if ((uint8_t)(e->x1 + 2) < mcx || e->x0 > (uint8_t)(mcx + 2)) continue;
        enemy_pos(e, t, &ex, &eyy, &len);
        if (e->kind == E_CRUSHER) {
            if (overlap((uint16_t)(hx + 1), (uint8_t)(hy + 1), HB_W - 2, HB_H - 2, (uint16_t)(ex + 2), eyy, 12, (uint8_t)(len + 16)))
                m->dead = DIE_CRUSHER;
        } else if (overlap((uint16_t)(hx + 1), (uint8_t)(hy + 1), HB_W - 2, HB_H - 2, (uint16_t)(ex + 3), (uint8_t)(eyy + 4), 10, 10)) {
            m->dead = DIE_DRONE;
        }
    }
    }
    if (m->dead) return (uint16_t)((ev & ~EV_EXIT) | EV_DEAD);
    return ev;
}
