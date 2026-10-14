/* ALIGNMENT - the mech at war (see world.h).
 *
 * Everything here is integer and deterministic: the same inputs give the same frames
 * on the Game Boy and on the host. Enemies keep moving as pure functions of the stage
 * clock (phys.c); all they gain here is armour, so they can be shot or stomped. A
 * stomp is the mech landing on an enemy along its own gravity: flip onto a ceiling
 * crawler and it is scrap. Bosses are small state machines in their arena. */
#ifdef __SDCC
#pragma bank 255
#endif
#include "world.h"
#include "mul8.h"

uint8_t near_n;
Near near[MAX_NEAR];
static uint8_t ns_first;         /* near_scan's cursor in the (sorted) enemies */
uint8_t ev2;
#ifdef __SDCC
#include <gb/gb.h>
uint8_t dbg_w[6];
#define MARK(k) (dbg_w[k] = LY_REG)
#else
#define MARK(k)
#endif
uint16_t ev_kx;
uint8_t ev_ky;

static Stage *S;
static Player *P;
static Boss *B;
static int8_t gd;               /* the mech's "down": +1 or -1 */
static uint16_t mx;             /* the mech's hitbox, shrunk a pixel each side, px */
static uint8_t my;
#define MW (HB_W - 2)
#define MH (HB_H - 2)

#define ALONG(v) (gd > 0 ? (int16_t)(v) : (int16_t)-(v))

void player_spawn(Player *p, uint8_t cx, uint8_t cy) CORE_BANKED
{
    uint8_t i;
    mech_spawn(&p->m, cx, cy, G_DOWN);
    for (i = 0; i < NSHOT; i++) p->shot[i].life = 0;
    p->hull = HULL_MAX;
    p->inv = 0;
    p->cd = 0;
}

/* ---------------------------------------------------------------- enemies */

/* a triangle wave on an 8-bit phase: 0..len..0 every 256 steps (no division) */
static uint16_t tri(uint8_t ph, uint8_t len)
{
    uint8_t t = ph < 128 ? ph : (uint8_t)(255 - ph);
    return (uint16_t)(mul8(t, len) >> 7);
}

/* where an enemy is at time t (here, in the world step's bank: it runs for every
 * enemy near the mech every frame, and a call to another bank costs) */
static void enemy_pos(const Enemy *e, uint16_t t, Near *q)
{
#define px (&q->x)
#define py (&q->y)
#define len (&q->len)
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
#undef px
#undef py
#undef len

static uint8_t bolt_at(const Enemy *e, uint16_t t, uint16_t *bx)
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

uint8_t turret_bolt(const Enemy *e, uint16_t t, uint16_t *bx) CORE_BANKED
{
    return bolt_at(e, t, bx);
}

/* ---------------------------------------------------------------- damage */

static uint16_t hurt(uint8_t why)
{
    if (P->inv || P->m.dead) return 0;
    if (P->strict) {
        P->m.dead = why;
        return 0;
    }
    if (P->shield) {
        P->shield = 0;
        P->inv = INV_FRAMES;
        ev2 |= EV2_SHIELD;
        return EV_HURT;
    }
    if (P->hull <= 1) {
        P->hull = 0;
        P->m.dead = why;
        return 0;
    }
    P->hull--;
    P->inv = INV_FRAMES;
    P->power = 0;               /* (a hit costs the power shot, as in the old games) */
    /* knocked off its feet */
    P->m.vy = ALONG(-32);
    P->m.ground = 0;
    return EV_HURT;
}

static void kill_at(uint16_t x, uint8_t y)
{
    ev_kx = x;
    ev_ky = y;
}

/* ---------------------------------------------------------------- the boss */

static uint16_t aL, aR;         /* the arena's inside, px: left, right (exclusive) */
static uint8_t aT, aB;          /* top (below the ceiling), bottom (the floor's surface) */

static void arena_box(void)
{
    const Arena *a = &S->arena;
    aL = (uint16_t)((uint16_t)(a->x0 + 1) << 4);
    /* (SDCC 4.4 stored a zero high byte for "aR = (uint16_t)a->x1 << 4": its register
     * tracking kept H = 0 through the shifts; so the two bytes are made by hand) */
    aR = (uint16_t)(((uint16_t)(uint8_t)(a->x1 >> 4) << 8) | (uint8_t)(a->x1 << 4));
    aT = (uint8_t)((a->top + 1) << 4);
    aB = (uint8_t)(a->bot << 4);
}

/* the boss's hurt box, into bbx / bby / bbw / bbh */
static uint16_t bbx;
static uint8_t bby, bbw, bbh;
static void boss_box(void)
{
    switch (B->kind) {
    case BK_TWIN: bbx = B->x + 3; bby = (uint8_t)(B->y + 1); bbw = 10; bbh = 14; break;
    case BK_HIVE: bbx = B->x + 4; bby = (uint8_t)(B->y + 4); bbw = 24; bbh = 24; break;
    case BK_PISTON: bbx = B->x + 2; bby = B->y; bbw = 28; bbh = 32; break;
    default: bbx = B->x + 2; bby = (uint8_t)(B->y + 4); bbw = 28; bbh = 24; break;
    }
}

void boss_reset(Boss *b, Stage *st) CORE_BANKED
{
    static const uint8_t hp1[NUM_BK] = { 0, 40, 24, 40, 32 };
    static const uint8_t hp2[NUM_BK] = { 0, 56, 32, 56, 44 };
    uint8_t i;
    const Arena *a = &st->arena;
    b->kind = a->kind;
    b->level = a->level;
    b->active = 0;
    b->dead = a->kind == BOSS_NONE;
    b->hpmax = b->hp = a->level > 1 ? hp2[a->kind] : hp1[a->kind];
    b->state = b->st = b->cycle = b->clock = 0;
    b->x = (uint16_t)a->bx << 4;
    b->y = (uint8_t)(a->by << 4);
    if (a->kind == BK_TWIN) b->y = (uint8_t)((a->by + 1) << 4);
    b->vx = b->vy = 0;
    b->g = G_DOWN;
    b->open = (uint8_t)(a->kind != BK_PISTON);
    b->flash = 0;
    b->memo_g = G_DOWN;
    b->flip_in = b->hop_cd = 0;
    for (i = 0; i < NBOLT; i++) b->b[i].life = 0;
    ns_first = 0;
}

static void boss_damage(uint8_t dmg)
{
    if (!B->open) {
        ev2 |= EV2_TINK;
        return;
    }
    B->flash = 8;
    if (B->hp > dmg) {
        B->hp = (uint8_t)(B->hp - dmg);
        ev2 |= EV2_BOSS_HIT;
        return;
    }
    B->hp = 0;
    B->dead = 1;
    B->active = 0;
    {
        uint8_t i;
        for (i = 0; i < NBOLT; i++) B->b[i].life = 0;
    }
    arena_open(S);
    ev2 |= EV2_BOSS_DIE;
}

static void boss_fire(uint16_t x, uint8_t y, int8_t vx, int8_t vy)
{
    uint8_t i;
    for (i = 0; i < NBOLT; i++) {
        Bolt *b = &B->b[i];
        if (b->life) continue;
        b->x = x;
        b->y = y;
        b->vx = vx;
        b->vy = vy;
        b->life = 120;
        ev2 |= EV2_BOSS_FIRE;
        return;
    }
}

static int8_t sign16(int16_t v)
{
    return v > 0 ? 1 : v < 0 ? -1 : 0;
}

/* a triangle wave 0..127..0 on an 8-bit phase */
static uint8_t tri8(uint8_t ph)
{
    return ph < 128 ? ph : (uint8_t)(255 - ph);
}

static void move_toward(uint16_t target, uint8_t speed)
{
    if (B->x + speed <= target) {
        B->x += speed;
        B->vx = 1;
    } else if (B->x >= target + speed) {
        B->x -= speed;
        B->vx = -1;
    }
}

static void sentinel(uint16_t mcx, uint8_t mcy)
{
    uint8_t floor_y = (uint8_t)(aB - 32), sp = B->level;
    switch (B->state) {
    case 0: /* walk to one end of the arena */
        move_toward((B->cycle & 1) ? (uint16_t)(aL + 8) : (uint16_t)(aR - 40), sp);
        B->st++;
        if (B->st > 30 && (B->x <= aL + 8 + sp || B->x + 40 >= aR - sp)) {
            B->state = 1;
            B->st = 0;
        }
        break;
    case 1: /* a fan of bolts at the mech */
        B->st++;
        if (B->st == 12 || B->st == 24 || B->st == 36) {
            int8_t vx = (int8_t)(mcx < B->x + 16 ? -2 : 2), vy;
            vy = (int8_t)(sign16((int16_t)mcy - (int16_t)(B->y + 16)) + (B->st == 12 ? -1 : B->st == 36 ? 1 : 0));
            if (B->level > 1) vx = (int8_t)(vx + vx / 2);
            boss_fire(B->x + 12, (uint8_t)(B->y + 12), vx, vy);
        }
        if (B->st >= 48) {
            B->cycle++;
            B->st = 0;
            B->state = (uint8_t)((B->cycle & 1) ? 2 : 0);
            if (B->state == 2) B->g ^= 1;
        }
        break;
    default: /* leap: gravity flips under it, it falls to the other side */
        if (B->g) {
            if (B->y > aT + 3) B->y = (uint8_t)(B->y - 3);
            else { B->y = aT; B->state = 0; }
        } else {
            if (B->y + 3 < floor_y) B->y = (uint8_t)(B->y + 3);
            else { B->y = floor_y; B->state = 0; }
        }
        break;
    }
}

static void piston(uint16_t mcx)
{
    switch (B->state) {
    case 0: /* track the mech along the ceiling */
        B->open = 0;
        B->st++;
        {
            uint16_t target = mcx > aL + 16 ? (uint16_t)(mcx - 16) : aL;
            if (target + 32 > aR) target = (uint16_t)(aR - 32);
            move_toward(target, B->level);
            if ((B->st > 50 && (B->x + 4 >= target && B->x <= target + 4)) || B->st > 140) {
                B->state = 1;
                B->st = 0;
                B->vy = 0;
            }
        }
        break;
    case 1: /* slam */
        if (B->vy < 8) B->vy++;
        if (B->y + 32 + B->vy >= aB) {
            B->y = (uint8_t)(aB - 32);
            B->state = 2;
            B->st = 0;
            ev2 |= EV2_SLAM;
        } else {
            B->y = (uint8_t)(B->y + B->vy);
        }
        break;
    case 2: /* stuck in the floor, core open; bolts skate both ways along the floor */
        B->open = 1;
        B->st++;
        if (B->st == 8 || (B->level > 1 && B->st == 30)) {
            boss_fire(B->x, (uint8_t)(aB - 12), -3, 0);
            boss_fire(B->x + 24, (uint8_t)(aB - 12), 3, 0);
        }
        if (B->st > (B->level > 1 ? 55 : 70)) {
            B->state = 3;
            B->open = 0;
        }
        break;
    default: /* back up */
        if (B->y > aT + 2) {
            B->y = (uint8_t)(B->y - 2);
        } else {
            B->y = aT;
            B->state = 0;
            B->st = 0;
        }
        break;
    }
}

static void hive(uint16_t mcx, uint8_t mcy)
{
    uint8_t c = ++B->clock, k;
    uint8_t rx = (uint8_t)(aR - aL - 48), ry = (uint8_t)(aB - aT - 48);
    B->x = (uint16_t)(aL + 8 + (mul8(tri8(c), rx) >> 7));
    B->y = (uint8_t)(aT + 8 + (mul8(tri8((uint8_t)(c + (c >> 1) + 64)), ry) >> 7));
    B->open = (uint8_t)((c & 127) >= (B->level > 1 ? 88 : 72));
    if ((c & 127) == 40 || (B->level > 1 && (c & 127) == 104)) {
        /* spores at the mech */
        int8_t vx = (int8_t)(sign16((int16_t)mcx - (int16_t)(B->x + 16)) * 2);
        int8_t vy = sign16((int16_t)mcy - (int16_t)(B->y + 16));
        if (!vx) vx = 2;
        for (k = 0; k < 3; k++)
            boss_fire(B->x + 12, (uint8_t)(B->y + 12), vx, (int8_t)(vy + (int8_t)k - 1));
    }
}

static void twin(uint16_t mcx, uint8_t mcy)
{
    int16_t dx = (int16_t)mcx - (int16_t)(B->x + 8);
    uint8_t sp = B->level, k;
    uint8_t floor_y = (uint8_t)(aB - 16);
    /* it copies the mech's flips, a moment late */
    if (P->m.g != B->memo_g) {
        B->memo_g = P->m.g;
        B->flip_in = (uint8_t)(B->level > 1 ? 12 : 20);
    }
    if (B->flip_in && !--B->flip_in) {
        B->g ^= 1;
        B->state = 1;
    }
    if (B->state == 1) {
        /* falling to the other side */
        if (B->g) {
            if (B->y > aT + 4) B->y = (uint8_t)(B->y - 4);
            else { B->y = aT; B->state = 0; }
        } else {
            if (B->y + 4 < floor_y) B->y = (uint8_t)(B->y + 4);
            else { B->y = floor_y; B->state = 0; }
        }
        return;
    }
    /* keep its distance */
    if (dx > 56 || (dx < 0 && dx > -40)) {
        if (B->x + 16 + sp < aR) B->x += sp;
        B->vx = 1;
    } else if (dx < -56 || (dx >= 0 && dx < 40)) {
        if (B->x > aL + sp) B->x -= sp;
        B->vx = -1;
    }
    B->vx = (int8_t)(dx < 0 ? -1 : 1);      /* (it always faces the mech) */
    /* hop over a shot coming at it */
    if (B->hop_cd) B->hop_cd--;
    if (B->vy == 0 && !B->hop_cd) {
        for (k = 0; k < NSHOT; k++) {
            const Shot *s = &P->shot[k];
            int16_t d = (int16_t)(B->x + 4) - (int16_t)s->x;
            if (s->life && ((s->vx > 0 && d > 0 && d < 40) || (s->vx < 0 && d < 0 && d > -40))) {
                B->vy = 6;
                B->hop_cd = 40;
                break;
            }
        }
    }
    if (B->vy) {
        /* a hop: up 6+5+...+1 then back down */
        int8_t step = (int8_t)(B->vy > 0 ? (B->vy > 3 ? 4 : 2) : -(B->vy < -3 ? 4 : 2));
        if (B->g) B->y = (uint8_t)(B->y + step);
        else B->y = (uint8_t)(B->y - step);
        B->vy--;
        if (B->vy < -6) {
            B->vy = 0;
            B->y = B->g ? aT : floor_y;
        }
    }
    /* fire when level with the mech */
    B->st++;
    if (B->st >= (B->level > 1 ? 32 : 48)) {
        int16_t dy = (int16_t)mcy - (int16_t)(B->y + 8);
        if (dy > -20 && dy < 20) {
            B->st = 0;
            boss_fire((uint16_t)(B->vx > 0 ? B->x + 12 : B->x - 4), (uint8_t)(B->y + 4),
                      (int8_t)(B->vx > 0 ? 3 : -3), 0);
        }
    }
}

static uint16_t boss_frame(void)
{
    uint16_t ev = 0, bx;
    uint8_t i, by, bw, bh;
    uint16_t mcx = (uint16_t)(mx + (MW >> 1));
    uint8_t mcy = (uint8_t)(my + (MH >> 1));
    if (B->flash) B->flash--;
    switch (B->kind) {
    case BK_SENTINEL: sentinel(mcx, mcy); break;
    case BK_PISTON: piston(mcx); break;
    case BK_HIVE: hive(mcx, mcy); break;
    default: twin(mcx, mcy); break;
    }
    /* its bolts */
    for (i = 0; i < NBOLT; i++) {
        Bolt *b = &B->b[i];
        if (!b->life) continue;
        b->life--;
        b->x = (uint16_t)(b->x + b->vx);
        b->y = (uint8_t)(b->y + b->vy);
        if (b->x + 8 < aL || b->x >= aR || b->y + 8 < aT || b->y >= aB) {
            b->life = 0;
            continue;
        }
        {
            uint16_t x = (uint16_t)(b->x + 2);
            uint8_t y = (uint8_t)(b->y + 2);
            if (mx < (uint16_t)(x + 4) && x < (uint16_t)(mx + MW) && my < (uint8_t)(y + 4) && y < (uint8_t)(my + MH)) {
                b->life = 0;
                ev |= hurt(DIE_BOLT);
            }
        }
    }
    /* touching it */
    boss_box();
    bx = bbx;
    by = bby;
    bw = bbw;
    bh = bbh;
    if (!P->m.dead && mx < (uint16_t)(bx + bw) && bx < (uint16_t)(mx + MW) && my < (uint8_t)(by + bh) &&
        by < (uint8_t)(my + MH)) {
        if (B->kind == BK_PISTON && B->state == 1) {
            P->m.dead = DIE_CRUSHER;
        } else if ((B->kind == BK_SENTINEL || B->kind == BK_TWIN) && ALONG(P->m.vy) >= 16 &&
                   (gd > 0 ? (uint8_t)(my + MH) <= (uint8_t)(by + 8) : my + 8 >= (uint8_t)(by + bh))) {
            /* stomped */
            boss_damage(3);
            P->m.vy = ALONG(-56);
            P->m.ground = 0;
            P->m.airflip = 1;
            ev |= EV_STOMP;
        } else {
            ev |= hurt(DIE_BOSS);
        }
    }
    return ev;
}

/* ---------------------------------------------------------------- one frame */



/* ---- the enemies near the mech
 * The per-enemy loop runs every frame for up to 8 enemies: on the Game Boy it is
 * hand-written (SDCC kept every loop variable on the stack: ~5 scanlines an enemy).
 * The C version is the reference, and the host's. Near entry: x 0-1, y 2, len 3, i 4,
 * kind 5, bx 6-7, by 8, bw 9, bh 10. */
#ifdef __SDCC
typedef char near_is_11_bytes[sizeof(Near) == 11 ? 1 : -1];
#endif
static Near nt;                 /* the enemy being looked at */
static const Enemy *ns_e;
static uint8_t ns_left, ns_lo, ns_hi, ns_i;
static uint16_t ns_t;

static void nt_pos(void)
{
    /* where the enemy ns_e is: into nt.x / nt.y / nt.len */
    const Enemy *e = ns_e;
    if (e->kind == E_TURRET) {
        /* (turrets don't move) */
        nt.x = (uint16_t)((uint16_t)(e->speed ? e->x1 : e->x0) << 4);
        nt.y = (uint8_t)(e->y0 << 4);
        nt.len = 0;
        if (!e->y1) nt.by = 4;      /* (on a floor: its box starts 4 px down) */
        else nt.by = 0;
    } else {
        enemy_pos(e, ns_t, &nt);
    }
}

#ifdef __SDCC
static void near_loop(void) __naked
{
    __asm
        ld  hl, #_near
    00010$:
        ld  a, (_ns_left)
        or  a
        ret z
        dec a
        ld  (_ns_left), a
        ; de = the enemy
        ld  a, (_ns_e)
        ld  e, a
        ld  a, (_ns_e + 1)
        ld  d, a
        push hl
        ; x1 < lo: skip; x0 > hi: done
        ld  h, d
        ld  l, e
        inc hl
        ld  a, (_ns_hi)
        cp  a, (hl)
        jp  c, 00090$           ; x0 > hi
        inc hl
        ld  a, (hl)             ; x1
        ld  hl, #_ns_lo
        cp  a, (hl)
        jp  c, 00080$           ; x1 < lo
        ld  hl, #7
        add hl, de
        ld  a, (hl)
        or  a
        jp  z, 00080$           ; destroyed
        ld  a, (de)
        cp  a, #3               ; E_TURRET: it stays put
        jp  nz, 00030$
        ld  hl, #5
        add hl, de
        ld  a, (hl)             ; speed (its direction): at x1 if it fires left
        ld  h, d
        ld  l, e
        inc hl
        or  a
        jp  z, 00031$
        inc hl
    00031$:
        ld  a, (hl)             ; its column
        ld  c, a
        swap a
        and a, #0xF0
        ld  (_nt), a
        ld  a, c
        swap a
        and a, #0x0F
        ld  (_nt + 1), a
        ld  hl, #3
        add hl, de
        ld  a, (hl+)            ; y0
        swap a
        and a, #0xF0
        ld  (_nt + 2), a
        xor a
        ld  (_nt + 3), a
        ld  a, (hl)             ; y1: on a ceiling?
        or  a
        ld  a, #4
        jp  z, 00032$
        xor a
    00032$:
        ld  (_nt + 8), a
        jp  00033$
    00030$:
        call    _nt_pos
    00033$:
        pop hl
        ; ---- the entry at hl: x, y, len, i, kind
        ld  a, (_nt)
        ld  c, a
        ld  (hl+), a
        ld  a, (_nt + 1)
        ld  b, a
        ld  (hl+), a            ; (bc = x)
        ld  a, (_nt + 2)
        ld  d, a
        ld  (hl+), a            ; (d = y)
        ld  a, (_nt + 3)
        ld  e, a
        ld  (hl+), a            ; (e = len)
        ld  a, (_ns_i)
        ld  (hl+), a
        push hl
        ld  hl, #_ns_e
        ld  a, (hl+)
        ld  h, (hl)
        ld  l, a
        ld  a, (hl)             ; kind
        pop hl
        ld  (hl+), a
        cp  a, #2               ; E_CRUSHER
        jp  z, 00040$
        cp  a, #3               ; E_TURRET
        jp  z, 00050$
        ; a drone: x + 3, y + 3, 10 x 10
        ld  a, c
        add a, #3
        ld  (hl+), a
        ld  a, b
        adc a, #0
        ld  (hl+), a
        ld  a, d
        add a, #3
        ld  (hl+), a
        ld  a, #10
        ld  (hl+), a
        ld  (hl+), a
        jp  00060$
    00040$:
        ; a crusher: x + 2, y, 12 x (len + 16)
        ld  a, c
        add a, #2
        ld  (hl+), a
        ld  a, b
        adc a, #0
        ld  (hl+), a
        ld  a, d
        ld  (hl+), a
        ld  a, #12
        ld  (hl+), a
        ld  a, e
        add a, #16
        ld  (hl+), a
        jp  00060$
    00050$:
        ; a turret: x + 2, y + (4 on a floor), 12 x 12
        ld  a, c
        add a, #2
        ld  (hl+), a
        ld  a, b
        adc a, #0
        ld  (hl+), a
        ld  a, (_nt + 8)
        add a, d
        ld  (hl+), a
        ld  a, #12
        ld  (hl+), a
        ld  (hl+), a
    00060$:
        ld  a, (_near_n)
        inc a
        ld  (_near_n), a
        cp  a, #MAX_NEAR
        ret z
        push hl
    00080$:
        pop hl
        ; the next enemy
        ld  a, (_ns_i)
        inc a
        ld  (_ns_i), a
        ld  a, (_ns_e)
        add a, #8
        ld  (_ns_e), a
        jp  nc, 00010$
        ld  a, (_ns_e + 1)
        inc a
        ld  (_ns_e + 1), a
        jp  00010$
    00090$:
        pop hl
        ret
    __endasm;
}
#endif


static void near_scan(uint16_t t, uint16_t px)
{
    /* the enemies within about a screen of the mech (stages keep them sorted by x0),
     * where they are and their boxes */
    uint8_t mcx = (uint8_t)(px >> 4), lo;
    lo = mcx > 6 ? (uint8_t)(mcx - 6) : 0;      /* (about the screen: the camera keeps */
    /* start from a cursor near the mech: nothing reaches more than 16 columns right of
     * where it starts (x0) */
    if (ns_first > S->nenemies) ns_first = 0;
    while (ns_first && (uint8_t)(S->enemy[ns_first - 1].x0 + 16) >= lo) ns_first--;
    while (ns_first < S->nenemies && (uint8_t)(S->enemy[ns_first].x0 + 16) < lo) ns_first++;
    ns_lo = lo;
    ns_hi = (uint8_t)(mcx + 7);                 /* the mech near the middle) */
    ns_t = t;
    ns_i = ns_first;
    ns_e = &S->enemy[ns_first];
    ns_left = (uint8_t)(S->nenemies - ns_first);
    near_n = 0;
#ifdef __SDCC
    near_loop();
#else
    {
        Near *q = near;
        for (; ns_left; ns_left--, ns_i++, ns_e++) {
            const Enemy *e = ns_e;
            if (e->x0 > ns_hi) break;
            if (e->x1 < ns_lo || !e->hp) continue;
            nt_pos();
            q->x = nt.x;
            q->y = nt.y;
            q->len = nt.len;
            q->i = ns_i;
            q->kind = e->kind;
            switch (e->kind) {
            case E_CRUSHER: q->bx = q->x + 2; q->by = q->y; q->bw = 12; q->bh = (uint8_t)(q->len + 16); break;
            case E_TURRET: q->bx = q->x + 2; q->by = (uint8_t)(q->y + nt.by); q->bw = 12; q->bh = 12; break;
            default: q->bx = q->x + 3; q->by = (uint8_t)(q->y + 3); q->bw = 10; q->bh = 10; break;
            }
            q++;
            if (++near_n == MAX_NEAR) break;
        }
    }
#endif
}

static uint16_t damage_enemy(const Near *q, uint8_t dmg)
{
    Enemy *e = &S->enemy[q->i];
    kill_at((uint16_t)(q->x + 8), (uint8_t)(q->y + 8));
    if (e->hp > dmg) {
        e->hp = (uint8_t)(e->hp - dmg);
        return EV_HIT;
    }
    e->hp = 0;
    return EV_KILL;
}

/* does the box (x, y, w, h) overlap the near enemy q's box? */
#define HITS_NEAR(q, x, y, w, h) ((x) < (uint16_t)((q)->bx + (q)->bw) && (q)->bx < (uint16_t)((x) + (w)) && \
                                  (y) < (uint8_t)((q)->by + (q)->bh) && (q)->by < (uint8_t)((y) + (h)))


/* what stops a shot, by gravity and terrain */
static const uint8_t shot_solid[2][NUM_T] = {
    { 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
};

static uint16_t shots(uint16_t clk)
{
    uint16_t ev = 0, cx, sx;
    uint8_t i, j, t, sy;
    Shot *s = P->shot;
    const Near *q;
    const uint8_t *cells = S->cell, *sol = shot_solid[P->m.g];
    if (B->active) boss_box();
    for (i = 0; i < NSHOT; i++, s++) {
        if (!s->life) continue;
        s->life--;
        s->x = (uint16_t)(s->x + s->vx);
        /* walls stop it (a phase block only in the gravity that makes it solid) */
        cx = (uint16_t)(s->x + 4);
        if ((uint8_t)(cx >> 4) >= S->w) { s->life = 0; continue; }
        t = cells[(cx & 0xFFF0u) | (uint8_t)((uint8_t)(s->y + 4) >> 4)];
        if (sol[t]) {
            s->life = 0;
            ev2 |= EV2_TINK;
            continue;
        }
        /* hits are looked for every other frame (a shot moves 8 px in two, less than
         * any box it could pass) */
        if ((uint8_t)((uint8_t)clk ^ i) & 1) continue;
        sx = (uint16_t)(s->x + 1);
        sy = (uint8_t)(s->y + 3);
        for (j = near_n, q = near; j; j--, q++) {
            if (!HITS_NEAR(q, sx, sy, 6, 2)) continue;
            if (!S->enemy[q->i].hp) continue;
            s->life = 0;
            if (q->kind == E_CRUSHER) ev2 |= EV2_TINK;
            else ev |= damage_enemy(q, P->power ? 2 : 1);
            break;
        }
        if (s->life && B->active && sx < (uint16_t)(bbx + bbw) && bbx < (uint16_t)(sx + 6) &&
            sy < (uint8_t)(bby + bbh) && bby < (uint8_t)(sy + 2)) {
            s->life = 0;
            boss_damage(P->power ? 2 : 1);
        }
    }
    return ev;
}

static uint16_t touch(uint16_t t)
{
    uint16_t ev = 0, bx;
    uint8_t j;
    const Near *q;
    for (j = near_n, q = near; j && !P->m.dead; j--, q++) {
        Enemy *e = &S->enemy[q->i];
        if (!e->hp) continue;
        /* a turret's bolt */
        if (q->kind == E_TURRET && my < (uint8_t)(q->y + 10) && (uint8_t)(q->y + 6) < (uint8_t)(my + MH) &&
            bolt_at(e, t, &bx)) {
            bx += 2;
            if (mx < (uint16_t)(bx + 4) && bx < (uint16_t)(mx + MW) && my < (uint8_t)(q->y + 10) &&
                (uint8_t)(q->y + 6) < (uint8_t)(my + MH))
                ev |= hurt(DIE_BOLT);
        }
        if (!HITS_NEAR(q, mx, my, MW, MH)) continue;
        if (q->kind == E_CRUSHER) {
            P->m.dead = DIE_CRUSHER;
        } else if (ALONG(P->m.vy) >= 16 &&
                   (gd > 0 ? (uint8_t)(my + MH) <= (uint8_t)(q->by + 7) : my + 7 >= (uint8_t)(q->by + q->bh))) {
            /* falling onto it along gravity: stomped flat, and a bounce */
            ev |= damage_enemy(q, 3) | EV_STOMP;
            P->m.vy = ALONG(-56);
            P->m.ground = 0;
            P->m.airflip = 1;
        } else {
            ev |= hurt(DIE_DRONE);
        }
    }
    return ev;
}

static uint16_t bomb(void)
{
    /* every armoured thing near the mech is scrap; a boss takes a heavy hit */
    uint16_t ev = 0;
    uint8_t j;
    P->bomb = 0;
    ev2 |= EV2_BOMB;
    for (j = 0; j < near_n; j++) {
        Enemy *e = &S->enemy[near[j].i];
        if (e->hp && e->kind != E_CRUSHER) {
            e->hp = 0;
            kill_at((uint16_t)(near[j].x + 8), (uint8_t)(near[j].y + 8));
            ev |= EV_KILL;
        }
    }
    if (B->active) {
        uint8_t o = B->open;
        B->open = 1;
        boss_damage(6);
        if (!B->dead) B->open = o;
    }
    return ev;
}

static void take_item(void)
{
    switch (ev_item) {
    case T_ITEM_P: P->power = 1; break;
    case T_ITEM_S: P->shield = 1; break;
    case T_ITEM_B: P->bomb = 1; break;
    default: if (P->hull < HULL_MAX) P->hull++; break;
    }
}

uint16_t world_step(Player *p, Stage *st, Boss *b, uint8_t in, uint16_t t) CORE_BANKED
{
    uint16_t ev, px;
    uint8_t py, i;
    Mech *m = &p->m;
    S = st;
    P = p;
    B = b;
    ev2 = 0;
    MARK(0);
    ev = mech_step(m, st, in, t);
    MARK(1);
    if (ev & EV_ITEM) take_item();
    if (p->inv) p->inv--;
    if (ev & EV_DEAD) return ev;
    gd = m->g ? -1 : 1;
    px = (uint16_t)(m->x >> 4);
    py = (uint8_t)(m->y >> 4);
    mx = (uint16_t)(px + HB_X + 1);
    my = (uint8_t)(py + HB_Y + 1);
    near_scan(t, px);
    MARK(2);
    if ((ev & EV_FLIP) && p->bomb) ev |= bomb();
    /* the cannon */
    if (p->cd) p->cd--;
    if ((in & IN_FIRE) && !p->cd) {
        Shot *s = p->shot;
        for (i = 0; i < NSHOT; i++, s++) {
            if (s->life) continue;
            s->life = SHOT_LIFE;
            s->vx = (int8_t)(p->power ? SHOT_SPEED + 1 : SHOT_SPEED);
            if (m->facing) s->vx = (int8_t)-s->vx;
            /* from the muzzle of the arm cannon */
            s->x = (uint16_t)(m->facing ? px - 4 : px + 12);
            s->y = (uint8_t)(py + 4);
            p->cd = (uint8_t)(p->power ? FIRE_CD + 3 : FIRE_CD);
            ev |= EV_SHOOT;
            break;
        }
    }
    ev |= shots(t);
    MARK(3);
    ev |= touch(t);
    MARK(4);
    /* the boss */
    if (!b->dead) {
        arena_box();
        if (b->active) {
            ev |= boss_frame();
        } else if (px >= aL) {
            arena_seal(st);
            b->active = 1;
            ev2 |= EV2_BOSS_ON;
        }
    }
    MARK(5);
    if (m->dead) ev = (uint16_t)((ev & ~(EV_EXIT | EV_HURT)) | EV_DEAD);
    return ev;
}
