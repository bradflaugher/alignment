/* ALIGNMENT - playing a stage: input, the world step, the camera and the streaming
 * map, drawing (the mech, its shots, enemies, bolts, explosions, the boss), effects,
 * the HUD, death and the checkpoint, the boss fight, the stage card and the tally. */
#pragma bank 255
#include <gb/gb.h>
#include <string.h>
#include "gfx.h"
#include "render.h"
#include "play.h"
#include "sound.h"
#include "save.h"
#include "assets.h"
#include "../core/stage.h"
#include "../core/phys.h"
#include "../core/world.h"

Run run;
Stage stage;                /* (globals: the ROM tests read them) */
Player player;
Boss boss;
#define mech player.m
uint16_t stage_t;           /* the stage clock: enemies and lasers run on it */
uint8_t play_state;         /* PS_*: what the game is doing */
uint16_t play_lag;          /* frames the loop missed (the ROM tests check it stays 0) */
uint8_t play_ly;            /* the scanline the last frame's work ended on (tests) */
uint8_t dbg_ly[8], dbg_e[4];
static uint8_t last_frame, lag_armed;

enum { PS_CARD = 0, PS_PLAY, PS_DEAD, PS_CLEAR, PS_PAUSE };

static uint8_t spawn_x, spawn_y;    /* respawn cell */
static uint8_t checked;             /* the checkpoint has been reached */
static uint8_t col_lo, col_hi;      /* stage columns currently in the BG ring */
static uint8_t pose, pose_t;        /* mech animation */
static uint8_t hud_cells, hud_lives, hud_bar, hud_g, hud_arms;
static uint8_t hud_thr[8];          /* column (or, in a fight, boss armour) for each half step */
static uint8_t fx_pend, fxp_x, fxp_y;   /* effects waiting for a quiet frame (fx_deferred) */
static uint8_t laser_was;
static uint8_t hud_msg_t;           /* a message is up in the HUD for this many frames */
static uint8_t mech_hidden;
static uint8_t demo_jheld;          /* (demo input: A held at the end of the last move) */
static uint8_t shoot_t;             /* frames since the cannon fired (the firing pose) */
static uint8_t hit_i, hit_t;        /* the enemy that was just hit flashes */
static uint8_t boss_boom;           /* frames of the boss's death throes left */
static uint8_t boss_seen;           /* the fight started this life (HUD bar) */

/* explosions, drawn in free enemy slots */
#define NBOOM 2
static uint16_t boom_x[NBOOM];
static uint8_t boom_y[NBOOM], boom_t[NBOOM], boom_next;

#define POSE_IDLE 0
#define POSE_RUN 1
#define POSE_AIR 2
#define POSE_FLIP 3
#define POSE_LAND 4
#define POSE_DEAD 5

static const char *const deck_names[8] = {
    "HANGAR", "REACTOR", "FOUNDRY", "CRYO", "ARRAY", "SPINE", "VAULT", "CORE"
};
static const char *const mech_names[NUM_MECH] = {
    "PLASMA PITS", "PLASMA", "STAGGERED PITS", "CRAWLER DRONES", "PHASE BLOCKS",
    "HOVER DRONES", "LASER GATES", "CRUSHERS", "SHAFTS", "GUN TURRETS"
};
static const char *const boss_names[NUM_BK] = {
    "", "SENTINEL", "PISTON", "THE HIVE", "MIRROR MECH"
};

/* ------------------------------------------------------------------ helpers */

static uint16_t mech_px(void) { return (uint16_t)(mech.x >> 4); }
static uint8_t mech_py(void) { return (uint8_t)(mech.y >> 4); }

/* a 16x16 sprite (four 8x8) at screen position (sx, sy), tiles spr_t.., attributes
 * spr_p (its flip bits mirror it) (off screen: hidden). gfx.c's spr16a (asm) writes it. */
static void spr16(uint8_t slot, int16_t sx, int16_t sy)
{
    if (sx < -15 || sx > 167 || sy < -15 || sy > 151) {
        OAM_item_t *o = &shadow_OAM[slot];
        o[0].y = o[1].y = o[2].y = o[3].y = 0;
        return;
    }
    spr_x = (uint8_t)(sx + 8);
    spr_y = (uint8_t)(sy + 16);
    spr16a(slot);
}
#define SPR(t, p) (spr_t = (t), spr_p = (p))
#define FLIPS(fx, fy) ((uint8_t)(((fx) ? S_FLIPX : 0) | ((fy) ? S_FLIPY : 0)))

/* a 32x32 picture: 16 sprites, tiles base + row * 4 + column, mirrored as needed (gfx.c's
 * spr32a writes them) */
static void spr32(int16_t sx, int16_t sy, uint8_t base, uint8_t fx, uint8_t fy, uint8_t prop)
{
    uint8_t r, cm = 0, rm = 0;
    uint16_t vis = 0xFFFF;
    if (sx > -8 && sx < 136 && sy > -16 && sy < 120) goto all;     /* (all of it on screen) */
    vis = 0;
    for (r = 0; r < 4; r++) {
        int16_t x = (int16_t)(sx + 8 + (r << 3)), y = (int16_t)(sy + 16 + (r << 3));
        if (x > 0 && x < 168) cm |= (uint8_t)(1 << r);
        if (y > 0 && y < 160) rm |= (uint8_t)(1 << r);
    }
    for (r = 0; r < 4; r++)
        if (rm & (1 << r)) vis |= (uint16_t)cm << (r << 2);
all:
    if (fx) prop |= S_FLIPX;
    if (fy) prop |= S_FLIPY;
    spr_x = (uint8_t)(sx + 8);
    spr_y = (uint8_t)(sy + 16);
    spr_t = base;
    spr_p = prop;
    spr_vis = vis;
    spr32a(SP_ENEMY);
}

static void hide4(uint8_t slot)
{
    OAM_item_t *o = &shadow_OAM[slot];
    o[0].y = o[1].y = o[2].y = o[3].y = 0;
}

/* ------------------------------------------------------------------ HUD */

/* the HUD is one window row kept in hud_t / hud_a and copied in VBlank (hud_commit):
 * a few byte stores here, no mid-frame VRAM waits, no generic text code.
 *   0 gravity | 1 lives icon, 2-3 lives | 5-7 hull | 8 shield | 9 power | 10 bomb |
 *   12 cell icon, 13-14 cells | 16-19 progress (or the boss's armour) */
#define HUD_CH(c) FONT_TILE(c)

static void hud_digits(uint8_t x, uint8_t v, uint8_t pal)
{
    uint8_t tens = 0;
    while (v >= 10) { v -= 10; tens++; }
    if (tens > 9) tens = 9;
    hud_t[x] = (uint8_t)(HUD_CH('0') + tens);
    hud_t[x + 1] = (uint8_t)(HUD_CH('0') + v);
    hud_a[x] = hud_a[x + 1] = pal;
}

static void hud_clear(uint8_t pal)
{
    uint8_t i;
    for (i = 0; i < 20; i++) {
        hud_t[i] = TILE_BLANK;
        hud_a[i] = pal;
    }
}

static void hud_full(void)
{
    hud_clear(PAL_UI);
    hud_t[1] = UI_BOLT;
    hud_a[1] = PAL_GREEN;
    hud_t[12] = UI_CELL;
    hud_a[12] = PAL_GREEN;
    hud_commit();
    hud_cells = hud_lives = hud_bar = hud_g = hud_arms = 0xFF;
}

/* the bar's thresholds: stage columns, or (in a fight) boss armour lost */
static void bar_setup(uint8_t total)
{
    uint8_t k;
    for (k = 0; k < 8; k++) hud_thr[k] = (uint8_t)(((uint16_t)(k + 1) * total + 7) >> 3);
    hud_bar = 0xFF;
}

static void hud_update(void)
{
    /* one item per frame at most */
    uint8_t b, i, t, arms;
    if (hud_g != mech.g) {
        hud_g = mech.g;
        hud_t[0] = mech.g ? UI_GRAV_UP : UI_GRAV_DOWN;
        hud_a[0] = mech.g ? PAL_GREEN : PAL_PURPLE;
        hud_commit();
        return;
    }
    if (hud_lives != run.lives) {
        hud_lives = run.lives;
        hud_digits(2, run.lives, run.lives <= 1 ? PAL_HAZARD : PAL_UI);
        hud_commit();
        return;
    }
    arms = (uint8_t)(player.hull | (player.shield << 2) | (player.power << 3) | (player.bomb << 4));
    if (hud_arms != arms) {
        hud_arms = arms;
        for (i = 0; i < HULL_MAX; i++) {
            hud_t[5 + i] = i < player.hull ? UI_HULL : UI_HULL_EMPTY;
            hud_a[5 + i] = player.hull <= 1 ? PAL_HAZARD : PAL_GREEN;
        }
        hud_t[8] = player.shield ? UI_SHIELD : TILE_BLANK;
        hud_a[8] = PAL_GREEN;
        hud_t[9] = player.power ? HUD_CH('P') : TILE_BLANK;
        hud_a[9] = PAL_HAZARD;
        hud_t[10] = player.bomb ? HUD_CH('B') : TILE_BLANK;
        hud_a[10] = PAL_PURPLE;
        hud_commit();
        return;
    }
    if (hud_cells != run.stage_cells) {
        hud_cells = run.stage_cells;
        hud_digits(13, run.stage_cells, run.stage_cells >= stage.ncells ? PAL_GREEN : PAL_UI);
        hud_commit();
        return;
    }
    if (frame & 3) return;
    /* progress through the stage, or what is left of the boss: 4 tiles, half steps */
    b = 0;
    t = boss_seen ? (uint8_t)(boss.hpmax - boss.hp) : (uint8_t)(mech_px() >> 4);
    while (b < 8 && t >= hud_thr[b]) b++;
    if (boss_seen) b = (uint8_t)(8 - b);
    if (b != hud_bar) {
        hud_bar = b;
        for (i = 0; i < 4; i++) {
            t = (uint8_t)(i * 2);
            hud_t[16 + i] = b >= (uint8_t)(t + 2) ? UI_BAR_FULL : b == (uint8_t)(t + 1) ? UI_BAR_HALF : UI_BAR_EMPTY;
            hud_a[16 + i] = boss_seen ? PAL_HAZARD : PAL_PURPLE;
        }
        hud_commit();
    }
}

static void hud_msg(const char *s, uint8_t pal)
{
    uint8_t n = (uint8_t)strlen(s), x;
    if (n > 20) n = 20;
    hud_clear(pal);
    for (x = (uint8_t)((20 - n) >> 1); *s && x < 20; x++, s++) hud_t[x] = HUD_CH(*s);
    hud_commit();
    hud_cells = hud_lives = hud_bar = hud_g = hud_arms = 0xFF;
}

static void hud_say(const char *s, uint8_t pal, uint8_t t)
{
    hud_msg(s, pal);
    hud_msg_t = t;
}

/* ------------------------------------------------------------------ camera & map */

static uint16_t cam_target_x(void)
{
    int16_t x = (int16_t)mech_px() - 72 + (mech.facing ? -12 : 12);
    int16_t maxx = (int16_t)((uint16_t)stage.w * 16 - 160);
    if (boss.active) {
        /* the arena is about a screen wide: hold it framed */
        int16_t lo = (int16_t)(((uint16_t)stage.arena.x0 << 4) + 8);
        int16_t hi = (int16_t)(((uint16_t)stage.arena.x1 << 4) + 24 - 160);
        if (hi < lo) hi = lo;
        if (x < lo) x = lo;
        if (x > hi) x = hi;
    }
    if (x < 0) x = 0;
    if (x > maxx) x = maxx;
    return (uint16_t)x;
}

static uint8_t cam_target_y(void)
{
    int16_t y = (int16_t)mech_py() - 60;
    if (y < 0) y = 0;
    if (y > SH * 16 - 136) y = SH * 16 - 136;
    return (uint8_t)y;
}

/* keep the columns around the camera in the 16-column ring */
static void stream(void)
{
    uint8_t want_lo = (uint8_t)(cam_x >> 4), want_hi;
    if (want_lo) want_lo--;
    want_hi = (uint8_t)(want_lo + 13);
    if (want_hi >= stage.w) want_hi = (uint8_t)(stage.w - 1);
    /* one column per frame at most: the camera never outruns that */
    if (col_hi < want_hi && col_queued() < 4) {
        col_hi++;
        render_column(&stage, col_hi);
        if (col_hi - col_lo > 14) col_lo++;
    } else if (col_lo > want_lo && col_queued() < 4) {
        col_lo--;
        render_column(&stage, col_lo);
        if (col_hi - col_lo > 14) col_hi--;
    }
}

static void map_fill(void)
{
    /* everything around the camera, now (the screen is black) */
    uint8_t c;
    col_lo = (uint8_t)(cam_x >> 4);
    if (col_lo) col_lo--;
    col_hi = (uint8_t)(col_lo + 13);
    if (col_hi >= stage.w) col_hi = (uint8_t)(stage.w - 1);
    ENABLE_RAM;
    stream_on = 1;
    for (c = col_lo; c <= col_hi; c++) render_column(&stage, c);
    while (col_queued()) wait_vbl_done();
}

/* a column's cells changed (arena doors): build it again */
static void column_changed(uint8_t cx)
{
    render_rebuild(&stage, cx, (uint8_t)(cx >= col_lo && cx <= col_hi));
}

/* d / 4 rounded towards zero, but never 0 for d != 0 (no division routine) */
static int16_t quarter(int16_t d)
{
    int16_t q = d >= 0 ? (int16_t)(d >> 2) : (int16_t)-((-d) >> 2);
    return q ? q : d;
}

static void camera(uint8_t snap)
{
    uint16_t tx = cam_target_x();
    uint8_t ty = cam_target_y();
    int16_t d;
    if (snap) {
        cam_x = tx;
        cam_y = ty;
    } else {
        /* ease towards the target, at most 4 px a frame (the streaming keeps up) */
        d = (int16_t)(tx - cam_x);
        d = quarter(d);
        if (d > 4) d = 4;
        if (d < -4) d = -4;
        cam_x = (uint16_t)(cam_x + d);
        d = (int16_t)ty - (int16_t)cam_y;
        d = quarter(d);
        if (d > 3) d = 3;
        if (d < -3) d = -3;
        cam_y = (uint8_t)(cam_y + d);
    }
    scroll_x = (uint8_t)cam_x;
    scroll_y = cam_y;
}

/* ------------------------------------------------------------------ drawing */

static void draw_mech(void)
{
    uint8_t tile, pal;
    int16_t sx = (int16_t)(mech_px() - cam_x) - shake_x;
    int16_t sy = (int16_t)mech_py() - (int16_t)cam_y - shake_y;
    /* hidden after a loss, blinking while it can't be hurt */
    if (mech_hidden || (player.inv && (player.inv & 4))) {
        hide4(SP_MECH);
        return;
    }
    switch (pose) {
    case POSE_RUN: tile = ((pose_t >> 3) & 1) ? SPR_MECH_WALK2 : SPR_MECH_WALK1; break;
    case POSE_AIR: tile = SPR_MECH_FALL; break;
    case POSE_FLIP: tile = SPR_MECH_FLIP; break;
    case POSE_LAND: tile = SPR_MECH_LAND; break;
    case POSE_DEAD: tile = SPR_MECH_DEAD; break;
    default: tile = shoot_t < 8 ? SPR_MECH_SHOOT : ((frame >> 5) & 1) ? SPR_MECH_IDLE2 : SPR_MECH_IDLE; break;
    }
    pal = pose == POSE_DEAD ? SPAL_DEAD : (mech.g == G_DOWN ? SPAL_MECH_P : SPAL_MECH_G);
    if (player.shield && (frame & 8)) pal = SPAL_WHITE;
    SPR(tile, (uint8_t)((pal) | FLIPS(mech.facing, mech.g == G_UP)));
    spr16(SP_MECH, sx, sy);
}

static void draw_shots(void)
{
    uint8_t i, oy;
    uint16_t ox;
    OAM_item_t *o = &shadow_OAM[SP_SHOT];
    const Shot *s = player.shot;
    uint16_t cx = (uint16_t)(cam_x + shake_x - 8);
    uint8_t cy = (uint8_t)(cam_y + shake_y - 16);
    uint8_t tile = player.power ? SPR_SHOT_POWER : SPR_SHOT;
    uint8_t pal = mech.g ? SPAL_SPARK_G : SPAL_SPARK_P;
    for (i = 0; i < NSHOT; i++, s++, o++) {
        /* (unsigned: anything off screen wraps past the limits) */
        ox = (uint16_t)(s->x - cx);
        oy = (uint8_t)(s->y - cy);
        if (!s->life || ox >= 168 || oy >= 160) {
            o->y = 0;
            continue;
        }
        o->y = oy;
        o->x = (uint8_t)ox;
        o->tile = tile;
        o->prop = (uint8_t)(pal | (s->vx < 0 ? S_FLIPX : 0));
    }
}

static OAM_item_t *bolt_o;
static uint8_t bolt_n;

static void bolt_draw(uint16_t x, uint8_t y)
{
    int16_t sx = (int16_t)(x - cam_x), sy = (int16_t)y - (int16_t)cam_y;
    if (bolt_n >= NUM_BOLT || sx < -8 || sx > 160 || sy < -8 || sy > 136) return;
    bolt_o->y = (uint8_t)(sy + 16 - shake_y);
    bolt_o->x = (uint8_t)(sx + 8 - shake_x);
    bolt_o->tile = SPR_BOLT;
    bolt_o->prop = SPAL_DEAD;
    bolt_o++;
    bolt_n++;
}

static void draw_enemies(void)
{
    /* from the world step's list of enemies near the mech (positions already worked out) */
    uint8_t j, n = 0, k, rods = 0, len, age;
    uint16_t bx;
    uint8_t ey;
    int16_t sx, sy;
    OAM_item_t *rod = &shadow_OAM[SP_ROD];
    const Near *q = near;
    uint16_t cx = (uint16_t)(cam_x + shake_x);
    uint8_t cy = (uint8_t)(cam_y + shake_y);
    bolt_o = &shadow_OAM[SP_BOLT];
    bolt_n = 0;
    for (j = near_n; j && n < NUM_ESPR; j--, q++) {
        const Enemy *e = &stage.enemy[q->i];
        if (!e->hp) continue;
        sx = (int16_t)(q->x - cx);
        if (sx < -16 || sx > 160) continue;
        ey = q->y;
        sy = (int16_t)ey - (int16_t)cy;
        len = q->len;
        if (q->kind == E_CRUSHER) {
            /* the piston head, and its rod above it */
            int16_t ry = (int16_t)(sy + len - 8);
            uint8_t rx = (uint8_t)(sx + 12);
            SPR(SPR_CRATE, (uint8_t)((SPAL_CRATE) | FLIPS(0, 0)));
            spr16((uint8_t)(SP_ENEMY + (n << 2)), sx, (int16_t)(sy + len));
            for (k = 0; k < NUM_ROD && rods < NUM_ROD; k++, ry -= 16) {
                if (ry < sy - 8) break;
                if (ry < -8 || ry > 151) continue;
                rod->y = (uint8_t)(ry + 16);
                rod->x = rx;
                rod->tile = SPR_STREAK;
                rod->prop = SPAL_CRATE;
                rod++;
                rods++;
            }
        } else {
            uint8_t t, fx, fy, pal = SPAL_BOT;
            if (q->kind == E_TURRET) {
                age = (uint8_t)((uint8_t)(stage_t + (uint16_t)(e->phase << 2)) & 127);
                t = age >= 8 && age < 16 ? SPR_TURRET_FIRE : SPR_TURRET;
                if ((age >> 1) == 4 && sx >= 0 && sx < 160) sfx_play(SFX_TURRET);
                fx = e->speed;
                fy = e->y1;
                if (age >= 8 && turret_bolt(e, stage_t, &bx)) bolt_draw(bx, (uint8_t)(ey + 4));
            } else {
                t = q->kind == E_HOVER ? ((frame >> 2) & 1 ? SPR_BOT_A : SPR_BOT_B)
                                       : ((frame >> 3) & 1 ? SPR_BOT_WALK : SPR_BOT_A);
                fx = (uint8_t)(q->x > mech_px());
                fy = (uint8_t)(q->kind == E_CRAWLER && e->y1);
            }
            if (hit_t && hit_i == q->i && (hit_t & 2)) pal = SPAL_WHITE;
            SPR(t, (uint8_t)((pal) | FLIPS(fx, fy)));
            spr16((uint8_t)(SP_ENEMY + (n << 2)), sx, sy);
        }
        n++;
    }
    /* explosions in the slots left */
    for (k = 0; k < NBOOM && n < NUM_ESPR; k++) {
        if (!boom_t[k]) continue;
        SPR(boom_t[k] > 8 ? SPR_EXPLODE1 : SPR_EXPLODE2, (uint8_t)(((uint8_t)((frame & 2) ? SPAL_DEAD : SPAL_WHITE)) | FLIPS((uint8_t)(boom_t[k] & 4), 0)));
        spr16((uint8_t)(SP_ENEMY + (n << 2)), (int16_t)(boom_x[k] - cx), (int16_t)boom_y[k] - (int16_t)cy);
        n++;
    }
    for (; n < NUM_ESPR; n++) hide4((uint8_t)(SP_ENEMY + (n << 2)));
    for (; rods < NUM_ROD; rods++, rod++) rod->y = 0;
    for (k = bolt_n; k < NUM_BOLT; k++, bolt_o++) bolt_o->y = 0;
}

static void draw_boss_bolts(void)
{
    uint8_t k;
    const Bolt *b = boss.b;
    bolt_o = &shadow_OAM[SP_BOLT];
    bolt_n = 0;
    for (k = 0; k < NBOLT; k++, b++)
        if (b->life) bolt_draw(b->x, b->y);
    for (k = bolt_n; k < NUM_BOLT; k++, bolt_o++) bolt_o->y = 0;
}

static void draw_boss_body(void)
{
    uint8_t k, pal, base, fx = 0, fy = 0, rods = 0;
    int16_t sx = (int16_t)(boss.x - cam_x) - shake_x;
    int16_t sy = (int16_t)boss.y - (int16_t)cam_y - shake_y;
    OAM_item_t *rod = &shadow_OAM[SP_ROD];
    pal = boss.flash & 2 ? SPAL_WHITE : SPAL_BOT;
    if (boss_boom && (frame & 2)) pal = SPAL_DEAD;
    if (boss.dead && !boss_boom) {
        for (k = 0; k < 16; k += 4) hide4((uint8_t)(SP_ENEMY + k));
    } else if (boss.kind == BK_TWIN) {
        uint8_t t = boss.state == 1 ? SPR_MECH_FLIP : (frame >> 3) & 1 ? SPR_MECH_WALK1 : SPR_MECH_WALK2;
        SPR(t, (uint8_t)((pal) | FLIPS((uint8_t)(boss.vx < 0), boss.g)));
        spr16(SP_ENEMY, sx, sy);
        for (k = 4; k < 16; k += 4) hide4((uint8_t)(SP_ENEMY + k));
    } else {
        switch (boss.kind) {
        case BK_SENTINEL:
            base = (uint8_t)(BOSS_TILE_BASE + (boss.state == 0 && ((frame >> 3) & 1) ? 16 : 0));
            fx = (uint8_t)(boss.vx > 0);
            fy = boss.g;
            break;
        case BK_PISTON:
            base = (uint8_t)(BOSS_TILE_BASE + (boss.open ? 16 : 0));
            /* the piston's rod, up to the ceiling */
            {
                int16_t ry = (int16_t)(sy - 8);
                int16_t top = (int16_t)(((uint16_t)(stage.arena.top + 1) << 4) - (int16_t)cam_y - shake_y);
                for (; rods < NUM_ROD && ry >= top - 4; ry -= 16) {
                    if (ry < -8 || ry > 136) continue;
                    rod->y = (uint8_t)(ry + 16);
                    rod->x = (uint8_t)(sx + 8 + 12);
                    rod->tile = SPR_STREAK;
                    rod->prop = SPAL_CRATE;
                    rod++;
                    rods++;
                }
            }
            break;
        default:
            base = (uint8_t)(BOSS_TILE_BASE + (boss.open ? 16 : 0));
            break;
        }
        spr32(sx, sy, base, fx, fy, pal);
    }
    for (; rods < NUM_ROD; rods++, rod++) rod->y = 0;
}

static void boom(uint16_t x, uint8_t y)
{
    uint8_t k = boom_next;
    boom_next = (uint8_t)((k + 1) & (NBOOM - 1));
    boom_x[k] = (uint16_t)(x - 8);
    boom_y[k] = (uint8_t)(y - 8);
    boom_t[k] = 16;
}

/* ------------------------------------------------------------------ frame */

static void frame_end(void)
{
    uint8_t k;
    dbg_ly[4] = LY_REG;
    draw_mech();
    draw_shots();
    dbg_ly[5] = LY_REG;
    /* (a DMG redraws the enemies every other frame: the time goes to the game itself) */
    if (stage.arena.kind && (boss.active || boss_boom)) {
        /* (a DMG draws the boss and its bolts on alternate frames) */
        if (is_cgb || (frame & 1)) draw_boss_body();
        if (is_cgb || !(frame & 1)) draw_boss_bolts();
    } else if (is_cgb || (frame & 1)) {
        draw_enemies();
    }
    dbg_ly[6] = LY_REG;
    for (k = 0; k < NBOOM; k++)
        if (boom_t[k]) boom_t[k]--;
    if (hit_t) hit_t--;
    if (shoot_t < 255) shoot_t++;
    play_ly = LY_REG;
    wait_vbl_done();
    /* a frame of play must take exactly one frame */
    if (play_state == PS_PLAY) {
        /* (from the second frame of play on: menus and fades take their time) */
        if (lag_armed && (uint8_t)(frame - last_frame) > 1) play_lag++;
        lag_armed = 1;
    } else {
        lag_armed = 0;
    }
    last_frame = frame;
    dbg_ly[7] = LY_REG;
    input_poll();
    fx_update();
}

static void frames(uint8_t n)
{
    while (n--) {
        stage_t++;
        frame_end();
    }
}

/* ------------------------------------------------------------------ the stage card */

static void stage_card(void)
{
    uint8_t d = (uint8_t)(stage.deck & 7);
    char name[17];
    play_state = PS_CARD;
    gfx_hide_sprites();
    part_clear();
    HIDE_WIN;
    move_win(7, 0);
    clear_win();
    SHOW_WIN;
    if (run.mode == MODE_CAMPAIGN) {
        txt(1, 5, 4, "SECTOR", PAL_DIM);
        txt_num(1, 12, 4, run.stage, 1, PAL_GREEN);
        sector_name((uint8_t)run.stage, name);
        txt(1, (uint8_t)((20 - strlen(name)) >> 1), 6, name, PAL_PURPLE);
    } else {
        txt(1, 5, 4, "STAGE", PAL_DIM);
        txt_num(1, 11, 4, run.stage > 999 ? 999 : run.stage, 3, PAL_UI);
        txt(1, 4, 6, "DECK", PAL_DIM);
        txt_num(1, 9, 6, (uint8_t)(d + 1), 2, PAL_GREEN);
        txt(1, 12, 6, deck_names[d], PAL_PURPLE);
        txt(1, 5, 15, "SEED", PAL_DIM);
        txt_hex(1, 10, 15, run.seed, PAL_DIM);
    }
    if (run.stage == 1) {
        txt(1, 3, 9, "A    JUMP", PAL_UI);
        txt(1, 3, 10, "B    FIRE", PAL_UI);
        txt(1, 3, 11, "UP+A FLIP GRAVITY", PAL_GREEN);
        txt(1, 2, 13, "LAND ON DRONES TO", PAL_DIM);
        txt(1, 2, 14, "CRUSH THEM", PAL_DIM);
    } else if (stage.featured != 0xFF) {
        txt(1, 4, 10, "NEW HAZARD:", PAL_HAZARD);
        txt(1, (uint8_t)((20 - strlen(mech_names[stage.featured])) >> 1), 11, mech_names[stage.featured], PAL_UI);
    }
    if (stage.arena.kind) {
        txt(1, 6, 12, "WARNING", PAL_HAZARD);
        txt(1, (uint8_t)((20 - strlen(boss_names[stage.arena.kind])) >> 1), 13, boss_names[stage.arena.kind], PAL_HAZARD);
    }
    fade_to(0);
}

static void music_for_stage(void)
{
    static const uint8_t songs[3] = { SONG_DECK_A, SONG_DECK_B, SONG_DECK_C };
    music_play(run.mode == MODE_ENDLESS && run.stage > 8 && (run.stage & 4) ? SONG_ZEN : songs[stage.deck % 3]);
}

/* ------------------------------------------------------------------ spawning */

static void load_boss_art(void)
{
    /* one boss's 32 tiles into sprite tiles 96..127 (the twin is a mech) */
    uint8_t k = stage.arena.kind;
    if (k != BOSS_NONE && k != BK_TWIN) gfx_load_boss((uint8_t)(k - BK_SENTINEL));
}

static void spawn(void)
{
    uint8_t f;
    uint8_t keep_shield = 0;
    (void)keep_shield;
    player_spawn(&player, spawn_x, spawn_y);
    player.shield = player.power = player.bomb = 0;
    /* settle onto the floor */
    for (f = 0; f < 24; f++) mech_step(&mech, &stage, 0, stage_t);
    pose = POSE_IDLE;
    mech_hidden = 0;
    shoot_t = 255;
    hit_t = 0;
    gfx_phase(G_DOWN);
    camera(1);
}

static void arena_restart(void)
{
    /* (after a loss, or a new stage) the boss waits, the doors are as they were */
    boss_reset(&boss, &stage);
    arena_reset(&stage);
    boss_boom = 0;
    boss_seen = 0;
    bar_setup(stage.w);
}

static void stage_begin(void)
{
    uint16_t t0;
    fade_to(4);
    stage_card();
    t0 = frame;
    stream_on = 0;
    if (run.mode == MODE_CAMPAIGN) stage_load(&stage, (uint8_t)run.stage);
    else stage_gen(&stage, run.seed, run.stage);
    render_prebuild(&stage);
    pal_set_deck(stage.deck);
    load_boss_art();
    /* keep the card up a moment */
    while ((uint8_t)(frame - t0) < (stage.arena.kind || run.stage == 1 ? 160 : 100)) {
        wait_vbl_done();
        input_poll();
        if (joy_new & (J_A | J_START)) break;
    }
    fade_to(4);
    HIDE_WIN;
    checked = 0;
    render_checked = 0;
    fx_pend = 0;
    spawn_x = stage.start_x;
    spawn_y = stage.start_y;
    stage_t = 0;
    demo_jheld = 0;
    run.stage_cells = 0;
    run.stage_time = 0;
    arena_restart();
    spawn();
    map_fill();
    move_win(7, HUD_WY);
    hud_full();
    SHOW_WIN;
    laser_was = 0xFF;
    hud_msg_t = 0;
    music_for_stage();
    frame_end();
    fade_to(0);
    play_state = PS_PLAY;
}

/* ------------------------------------------------------------------ events */

static void land_fx(void)
{
    uint8_t y = mech.g ? (uint8_t)(mech_py()) : (uint8_t)(mech_py() + 15);
    /* (dust only for real landings on a DMG: hops are free) */
    if (is_cgb || mech.land_v >= 12) {
        part_spawn((uint16_t)(mech_px() + 4), y, -6, mech.g ? 2 : -2, SPR_PIXEL, SPAL_WHITE, 12);
        part_spawn((uint16_t)(mech_px() + 11), y, 6, mech.g ? 2 : -2, SPR_PIXEL, SPAL_WHITE, 12);
    }
    if (mech.land_v >= 12) {
        fx_shake(mech.land_v >= 16 ? 2 : 1);
        sfx_play(SFX_LAND);
    } else {
        sfx_play(SFX_STEP);
    }
}

static void flip_fx(void)
{
    uint8_t i;
    sfx_play(SFX_FLIP);
    flash(3, 0);
    gfx_phase(mech.g);
    {
        /* streaks rushing the new way down (one random byte each, cheap on a DMG) */
        int8_t vy = mech.g ? -40 : 40;
        uint8_t pal = mech.g ? SPAL_SPARK_G : SPAL_SPARK_P;
        uint8_t y0 = (uint8_t)(cam_y + (mech.g ? 100 : 8));
        uint16_t x0 = cam_x + 8;
        for (i = is_cgb ? 1 : 3; i < 5; i++) {
            uint8_t r = fx_rand();
            part_spawn(x0 + (uint8_t)((r & 127) + (i << 4)), (uint8_t)(y0 + (r >> 3)), 0, vy,
                       SPR_STREAK, pal, (uint8_t)(10 + (r & 7)));
        }
    }
}

/* the heavier halves of event effects (a burst, a HUD message) wait for a frame with
 * nothing else going on, one per frame: a DMG has no time for two at once */
#define FXP_BURST_CELL 0x01
#define FXP_BURST_CHECK 0x02
#define FXP_MSG_CHECK 0x04
#define FXP_MSG_LIFE 0x08
#define FXP_MSG_ITEM 0x10

static uint8_t fxp_item;

static void fx_deferred(void)
{
    if (fx_pend & FXP_BURST_CELL) {
        fx_pend &= (uint8_t)~FXP_BURST_CELL;
        part_burst((uint16_t)(fxp_x * 16 + 4), (uint8_t)(fxp_y * 16 + 4), 6, SPR_STAR, SPAL_SPARK_G, 8, 0);
    } else if (fx_pend & FXP_BURST_CHECK) {
        fx_pend &= (uint8_t)~FXP_BURST_CHECK;
        part_burst((uint16_t)(stage.check_x * 16 + 4), (uint8_t)(stage.check_y * 16), 10, SPR_SPARK_BIG,
                   SPAL_SPARK_G, 10, -1);
    } else if (fx_pend & FXP_MSG_CHECK) {
        fx_pend &= (uint8_t)~FXP_MSG_CHECK;
        hud_say("CHECKPOINT", PAL_GREEN, 60);
    } else if (fx_pend & FXP_MSG_LIFE) {
        fx_pend &= (uint8_t)~FXP_MSG_LIFE;
        hud_say("EXTRA LIFE", PAL_GREEN, 60);
    } else if (fx_pend & FXP_MSG_ITEM) {
        static const char *const items[4] = { "POWER SHOT", "SHIELD", "GRAVITY BOMB", "HULL REPAIRED" };
        fx_pend &= (uint8_t)~FXP_MSG_ITEM;
        hud_say(items[fxp_item - T_ITEM_P], PAL_GREEN, 60);
    }
}

static void cell_fx(void)
{
    render_cell(&stage, ev_cx, ev_cy);
    fxp_x = ev_cx;
    fxp_y = ev_cy;
    fx_pend |= FXP_BURST_CELL;
    if (run_cell(&run)) {
        sfx_play(SFX_NEWMECH);
        fx_pend |= FXP_MSG_LIFE;
    } else {
        sfx_play(SFX_CELL);
    }
}

static void item_fx(void)
{
    render_cell(&stage, ev_cx, ev_cy);
    fxp_x = ev_cx;
    fxp_y = ev_cy;
    fxp_item = ev_item;
    fx_pend |= FXP_BURST_CELL | FXP_MSG_ITEM;
    sfx_play(SFX_ITEM);
    flash(2, 0);
}

static void check_fx(void)
{
    if (checked) return;
    checked = 1;
    render_checked = 1;
    render_cell(&stage, stage.check_x, stage.check_y);
    spawn_x = stage.check_x;
    spawn_y = stage.check_y;
    sfx_play(SFX_EXIT_OPEN);
    fx_pend |= FXP_BURST_CHECK | FXP_MSG_CHECK;
}

static void boss_on(void)
{
    /* the door slams; respawns are at the door from now on */
    column_changed(stage.arena.x0);
    spawn_x = (uint8_t)(stage.arena.x0 - 1);
    spawn_y = (uint8_t)(stage.arena.bot - 1);
    boss_seen = 1;
    bar_setup(boss.hpmax);
    sfx_play(SFX_ALARM);
    fx_shake(3);
    music_play(run.mode == MODE_CAMPAIGN && run.stage == sector_count() ? SONG_FINAL : SONG_BOSS);
    hud_say("WARNING", PAL_HAZARD, 90);
}

static void boss_down(void)
{
    sfx_play(SFX_BOSS_DIE);
    music_play(SONG_CLEAR);
    run_score(&run, SCORE_BOSS);
    column_changed(stage.arena.x1);
    boss_boom = 96;
    fx_shake(6);
    flash(6, 1);
}

static void boss_throes(void)
{
    /* explosions all over the wreck, then it is gone */
    if ((boss_boom & 7) == 0) {
        uint8_t r = fx_rand();
        uint8_t w = boss.kind == BK_TWIN ? 16 : 32;
        boom((uint16_t)(boss.x + (r & (uint8_t)(w - 1))), (uint8_t)(boss.y + ((r >> 3) & (uint8_t)(w - 1))));
        part_burst((uint16_t)(boss.x + (w >> 1)), (uint8_t)(boss.y + (w >> 1)), 4, SPR_SPARK_BIG, SPAL_DEAD, 12, 0);
        fx_shake(2);
        if ((boss_boom & 15) == 0) sfx_play(SFX_EXPLODE);
    }
    if (!--boss_boom) {
        hud_say("TARGET DESTROYED", PAL_GREEN, 90);
        boss_seen = 0;
        bar_setup(stage.w);
    }
}

/* returns 1 if the run is over */
static uint8_t death(void)
{
    uint8_t i, over;
    play_state = PS_DEAD;
    pose = POSE_DEAD;
    sfx_play(mech.dead == DIE_PLASMA || mech.dead == DIE_LASER ? SFX_ZAP : SFX_CRUSH);
    fx_shake(4);
    flash(4, 1);
    music_set_rewind(1);
    hud_msg(mech.dead == DIE_PLASMA ? "PLASMA BREACH" : mech.dead == DIE_LASER ? "LASER BURN" :
            mech.dead == DIE_CRUSHER ? "HULL CRUSHED" : mech.dead == DIE_BOLT ? "HULL BREACHED" :
            mech.dead == DIE_BOSS ? "MECH DESTROYED" : "DRONE CONTACT", PAL_HAZARD);
    for (i = 0; i < 50; i++) {
        if ((i & 7) == 0)
            part_burst((uint16_t)(mech_px() + 4), (uint8_t)(mech_py() + 4), 5, SPR_SPARK_BIG, SPAL_DEAD, 12, 0);
        if (i == 4) boom((uint16_t)(mech_px() + 8), (uint8_t)(mech_py() + 8));
        if (i == 20) mech_hidden = 1;      /* (the wreck vanishes) */
        frames(1);
    }
    music_set_rewind(0);
    over = run_death(&run);
    if (over) return 1;
    fade_to(4);
    part_clear();
    /* everything comes back: the enemies, and a waiting boss */
    stage_rearm(&stage);
    if (stage.arena.kind && !boss.dead) {
        uint8_t was = boss.active;
        arena_restart();
        if (was) {
            render_rebuild(&stage, stage.arena.x0, 0);
            music_for_stage();
        }
    }
    spawn();
    map_fill();
    hud_full();
    frame_end();
    fade_to(0);
    play_state = PS_PLAY;
    return 0;
}

/* the exit: walk out, tally */
static void clear_stage(void)
{
    uint16_t bonus;
    uint8_t i, all = (uint8_t)(stage.ncells && run.stage_cells >= stage.ncells);
    play_state = PS_CLEAR;
    sfx_play(SFX_EXIT);
    for (i = 0; i < 30; i++) {
        mech.x += RUN_MAX;
        pose = POSE_RUN;
        pose_t++;
        frames(1);
    }
    bonus = run_clear(&run, stage.w, stage.ncells);
    save.total_stages++;
    save.total_cells += run.stage_cells;
    if (run.mode == MODE_CAMPAIGN) {
        if (run.stage > save.sector && run.stage <= sector_count()) save.sector = (uint8_t)run.stage;
        if (run.stage > sector_count()) save.campaign_done = 1;
    } else if (run.stage > save.best_stage) {
        save.best_stage = run.stage;
    }
    if (run.score > save.best_score) save.best_score = run.score;
    stream_on = 0;
    save_write();
    if (!stage.arena.kind) music_play(SONG_CLEAR);
    sfx_play(all ? SFX_OPTIMAL : SFX_SELECT);
    HIDE_WIN;
    move_win(7, 32);
    clear_win();
    draw_box(1, 2, 0, 16, 9, PAL_PURPLE);
    txt(1, 4, 1, run.mode == MODE_CAMPAIGN ? "SECTOR CLEAR" : "STAGE CLEAR", PAL_GREEN);
    put_tile(1, 4, 3, UI_CELL, PAL_GREEN);
    txt_num(1, 6, 3, run.stage_cells, 2, PAL_UI);
    txt(1, 8, 3, "/", PAL_DIM);
    txt_num(1, 9, 3, stage.ncells, 2, PAL_UI);
    if (all) txt(1, 12, 3, "ALL!", PAL_GREEN);
    txt(1, 4, 5, "BONUS", PAL_DIM);
    txt_num(1, 10, 5, bonus, 5, PAL_UI);
    txt(1, 4, 7, "SCORE", PAL_DIM);
    txt_num(1, 10, 7, (uint16_t)(run.score > 65535u ? 65535u : run.score), 5, PAL_GREEN);
    SHOW_WIN;
    for (i = 0; i < 200; i++) {
        frames(1);
        if (i > 30 && (joy_new & (J_A | J_START))) break;
    }
}

/* ------------------------------------------------------------------ pause */

/* 0 resume, 1 quit */
static uint8_t pause_menu(void)
{
    uint8_t sel = 0, r = 0xFF;
    play_state = PS_PAUSE;
    music_set_muffle(1);
    sfx_play(SFX_SELECT);
    HIDE_WIN;
    move_win(7, 48);
    clear_win();
    draw_box(1, 3, 0, 14, 7, PAL_PURPLE);
    txt(1, 6, 0, " PAUSED ", PAL_GREEN);
    txt(1, 6, 2, "RESUME", PAL_UI);
    txt(1, 6, 3, "QUIT", PAL_UI);
    if (run.mode == MODE_ENDLESS) {
        txt(1, 5, 5, "SEED", PAL_DIM);
        txt_hex(1, 10, 5, run.seed, PAL_UI);
    }
    SHOW_WIN;
    while (r == 0xFF) {
        put_tile(1, 5, 2, sel == 0 ? FONT_TILE('>') : TILE_BLANK, PAL_GREEN);
        put_tile(1, 5, 3, sel == 1 ? FONT_TILE('>') : TILE_BLANK, PAL_GREEN);
        wait_vbl_done();
        input_poll();
        fx_update();
        if (joy_new & (J_UP | J_DOWN)) { sel ^= 1; sfx_play(SFX_MENU); }
        if (joy_new & (J_START | J_B)) r = 0;
        if (joy_new & J_A) r = sel;
    }
    sfx_play(SFX_SELECT);
    HIDE_WIN;
    move_win(7, HUD_WY);
    hud_full();
    SHOW_WIN;
    music_set_muffle(0);
    play_state = PS_PLAY;
    return r;
}

/* ------------------------------------------------------------------ the loop */

/* Demo input: moves (one per 4 frames of the stage clock, see tests/physbot.c) instead
 * of the joypad, so a replay never depends on frame timing. The ROM tests load the host
 * bot's moves here. The cannon is held down all the time, as the bot plays. */
#define DEMO_MAX 1536
uint8_t demo_on;
uint16_t demo_len;
uint8_t demo_buf[DEMO_MAX];
static const uint8_t demo_acts[9][2] = {
    { 0, 0 }, { IN_L, 0 }, { IN_R, 0 },
    { IN_JUMP, IN_JUMP_NEW }, { IN_L | IN_JUMP, IN_JUMP_NEW }, { IN_R | IN_JUMP, IN_JUMP_NEW },
    { 0, IN_FLIP_NEW }, { IN_L, IN_FLIP_NEW }, { IN_R, IN_FLIP_NEW } };

static uint8_t demo_bits(void)
{
    uint16_t k = (uint16_t)(stage_t >> 2);
    uint8_t a, in, press;
    if (k >= demo_len) return IN_FIRE;
    a = demo_buf[k];
    in = demo_acts[a][0];
    press = (stage_t & 3) ? 0 : demo_acts[a][1];
    if ((press & IN_JUMP_NEW) && demo_jheld) press &= (uint8_t)~IN_JUMP_NEW;
    if ((stage_t & 3) == 3) demo_jheld = (uint8_t)((in & IN_JUMP) != 0);
    return (uint8_t)(in | press | IN_FIRE);
}

/* Metal Storm's buttons: A jumps, B fires, Up (or Down) + A turns gravity over;
 * SELECT flips too */
static uint8_t input_bits(void)
{
    if (demo_on) return demo_bits();
    {
    uint8_t j = joy, n = joy_new, in = 0;
    if (j & J_LEFT) in = IN_L;
    else if (j & J_RIGHT) in = IN_R;
    if (j & J_B) in |= IN_FIRE;
    if (j & J_A) in |= IN_JUMP;
    if (n & J_A) in |= (j & (J_UP | J_DOWN)) ? IN_FLIP_NEW : IN_JUMP_NEW;
    if (n & J_SELECT) in |= IN_FLIP_NEW;
    return in;
    }
}

static void kill_fx(uint8_t stomp)
{
    boom(ev_kx, ev_ky);
    sfx_play(SFX_EXPLODE);
    part_burst(ev_kx, ev_ky, 4, SPR_SPARK_BIG, SPAL_DEAD, 12, 0);
    run_score(&run, stomp ? SCORE_KILL * 2 : SCORE_KILL);
}

uint8_t play_run(void) BANKED
{
    uint8_t in;
    uint16_t ev;
    gfx_load_play_tiles();
    stage_begin();
    for (;;) {
        /* input (START first: SDCC 4.4 miscompiled a bit test that came after the
         * others here, opening the pause menu on A) */
        if (joy_new & J_START) {
            if (pause_menu()) {
                music_stop();
                stream_on = 0;
                return PLAY_QUIT;
            }
            continue;
        }
        in = input_bits();
        dbg_ly[0] = LY_REG;
        /* the world */
        stage_t++;
        if (run.stage_time < 65535u) run.stage_time++;
        ev = world_step(&player, &stage, &boss, in, stage_t);
        dbg_e[0] = LY_REG;
        if (ev & EV_FLIP) {
            flip_fx();
            pose = POSE_FLIP;
            pose_t = 0;
        }
        if (ev & EV_NOFLIP) sfx_play(SFX_ERROR);
        if (ev & EV_JUMP) sfx_play(SFX_JUMP);
        if (ev & EV_SHOOT) {
            sfx_play(player.power ? SFX_SHOOT_POWER : SFX_SHOOT);
            shoot_t = 0;
        }
        if (ev & EV_LAND) {
            land_fx();
            pose = POSE_LAND;
            pose_t = 0;
        } else if ((ev & EV_STEP) && !(ev & EV_SHOOT)) {
            sfx_play(SFX_STEP);
        }
        if (ev & EV_KILL) {
            kill_fx((uint8_t)((ev & EV_STOMP) != 0));
            if (ev & EV_STOMP) fx_shake(2);
        } else if (ev & EV_HIT) {
            sfx_play(SFX_HIT);
            hit_i = near[0].i;
            {
                uint8_t j;
                for (j = 0; j < near_n; j++)
                    if (near[j].x + 8 == ev_kx && near[j].y + 8 == ev_ky) hit_i = near[j].i;
            }
            hit_t = 8;
        }
        dbg_e[1] = LY_REG;
        if (ev & EV_HURT) {
            sfx_play(SFX_HURT);
            fx_shake(3);
            flash(2, 1);
        }
        if (ev2) {
            if (ev2 & EV2_BOMB) {
                sfx_play(SFX_BOMB);
                fx_shake(6);
                flash(8, 1);
            }
            if (ev2 & EV2_BOSS_ON) boss_on();
            if (ev2 & EV2_BOSS_DIE) boss_down();
            else if (ev2 & EV2_BOSS_HIT) sfx_play(SFX_BOSS_HIT);
            if (ev2 & EV2_SHIELD) sfx_play(SFX_HURT);
            if (ev2 & EV2_SLAM) {
                fx_shake(4);
                sfx_play(SFX_CRUSH);
            }
            if ((ev2 & EV2_BOSS_FIRE) && !(ev & EV_SHOOT)) sfx_play(SFX_TURRET);
        }
        dbg_e[2] = LY_REG;
        if (ev & (EV_CELL | EV_CHECK | EV_ITEM)) {
            if (ev & EV_CELL) cell_fx();
            if (ev & EV_ITEM) item_fx();
            if (ev & EV_CHECK) check_fx();
        } else if (fx_pend && !(ev & (EV_FLIP | EV_LAND | EV_KILL))) {
            fx_deferred();
        }
        dbg_ly[1] = LY_REG;
        if (boss_boom) boss_throes();
        if (ev & EV_DEAD) {
            if (death()) {
                music_stop();
                stream_on = 0;
                return PLAY_OVER;
            }
            continue;
        }
        if (ev & EV_EXIT) {
            clear_stage();
            if (run.mode == MODE_CAMPAIGN && run.stage > sector_count()) {
                stream_on = 0;
                return PLAY_WIN;
            }
            stage_begin();
            continue;
        }
        /* the mech's pose */
        pose_t++;
        if (pose == POSE_FLIP && pose_t < 10) {
        } else if (pose == POSE_LAND && pose_t < 6) {
        } else if (!mech.ground) {
            pose = POSE_AIR;
        } else if (mech.vx) {
            if (pose != POSE_RUN) pose_t = 0;
            pose = POSE_RUN;
        } else {
            pose = POSE_IDLE;
        }
        /* laser gates blink */
        if (laser_on(stage_t) != laser_was) {
            laser_was = laser_on(stage_t);
            gfx_laser(laser_was);
        }
        dbg_ly[2] = LY_REG;
        camera(0);
        stream();
        dbg_ly[3] = LY_REG;
        if (hud_msg_t) {
            if (!--hud_msg_t) hud_full();
        } else {
            hud_update();
        }
        frame_end();
    }
}
