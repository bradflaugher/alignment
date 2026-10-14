/* ALIGNMENT - playing: input, the turn animation, rewind, hints, the HUD, the pause
 * menu, the station map and the scroll from one room to the next. */
#pragma bank 255
#include <gb/gb.h>
#include <string.h>
#include "gfx.h"
#include "render.h"
#include "play.h"
#include "bgjob.h"
#include "sound.h"
#include "save.h"
#include "../core/room.h"
#include "../core/gen.h"

Run run;
uint8_t play_idle;
uint16_t scan_frames;               /* frames spent waiting for the generator (tests) */                 /* waiting for input (the ROM tests watch it) */
int16_t map_rx[MAP_N], map_ry[MAP_N];
uint8_t map_n, map_head;

#define HIST_N 64
Room room;              /* (global: the ROM tests read them) */
Door door;
State st;
static State hist[HIST_N];
static uint8_t hist_top, hist_n;
static TurnLog lg;

/* what is drawn: object pixel positions inside the room (int16: they leave it) */
static int16_t dpx[NOBJ], dpy[NOBJ];
static uint8_t dvis[NOBJ];          /* sprite shown */
static uint8_t dmove[NOBJ];         /* moving this frame (walk / fall frames) */
static uint8_t crate_spr;           /* crates drawn as sprites (bit per crate) */
static uint8_t facing;              /* 1 = left */
static uint8_t vis_g;               /* the gravity the sprites are drawn for */
static uint8_t mech_pose;           /* 0 idle, 1 walk, 2 fall, 3 flip, 4 land, 5 dead */
static uint8_t pose_t;
static uint8_t room_x8, room_y8;    /* the room's origin in map pixels */
static uint8_t hint_shown, hint_act;
static uint8_t queued;              /* buffered action, 0xFF none */
static uint8_t rewinding;

#define POSE_IDLE 0
#define POSE_WALK 1
#define POSE_FALL 2
#define POSE_FLIP 3
#define POSE_LAND 4
#define POSE_DEAD 5

static const char *const deck_names[8] = {
    "HANGAR", "REACTOR", "FOUNDRY", "CRYO", "ARRAY", "SPINE", "VAULT", "CORE"
};

/* ------------------------------------------------------------------ sprites */

static void spr16(uint8_t slot, int16_t px, int16_t py, uint8_t tile, uint8_t fx, uint8_t fy, uint8_t prop)
{
    uint8_t q, col, row, sc, sr;
    int16_t sx = (int16_t)(uint8_t)(room_x8 - scroll_x) + px - shake_x;
    int16_t sy = (int16_t)(uint8_t)(room_y8 - scroll_y) + py - shake_y;
    /* the room origin difference wraps mod 256: bring it into -96..159 */
    if (sx > 200) sx -= 256;
    if (sy > 200) sy -= 256;
    if (sx < -16 || sx >= 168 || sy < -16 || sy >= 160) {
        for (q = 0; q < 4; q++) move_sprite((uint8_t)(slot + q), 0, 0);
        return;
    }
    if (fx) prop |= S_FLIPX;
    if (fy) prop |= S_FLIPY;
    for (q = 0; q < 4; q++) {
        col = (uint8_t)(q & 1);
        row = (uint8_t)(q >> 1);
        sc = fx ? (uint8_t)(1 - col) : col;
        sr = fy ? (uint8_t)(1 - row) : row;
        set_sprite_tile((uint8_t)(slot + q), (uint8_t)(tile + sr * 2 + sc));
        set_sprite_prop((uint8_t)(slot + q), prop);
        move_sprite((uint8_t)(slot + q), (uint8_t)(sx + col * 8 + 8), (uint8_t)(sy + row * 8 + 16));
    }
}

static void spr_hide4(uint8_t slot)
{
    uint8_t q;
    for (q = 0; q < 4; q++) move_sprite((uint8_t)(slot + q), 0, 0);
}

static void draw_objects(void)
{
    uint8_t i, tile, fy = vis_g == G_UP;
    /* the mech */
    if (dvis[0]) {
        switch (mech_pose) {
        case POSE_WALK: tile = (pose_t & 4) ? SPR_MECH_WALK2 : SPR_MECH_WALK1; break;
        case POSE_FALL: tile = SPR_MECH_FALL; break;
        case POSE_FLIP: tile = SPR_MECH_FLIP; break;
        case POSE_LAND: tile = SPR_MECH_LAND; break;
        case POSE_DEAD: tile = SPR_MECH_DEAD; break;
        default: tile = ((frame >> 5) & 1) ? SPR_MECH_IDLE2 : SPR_MECH_IDLE; break;
        }
        spr16(SP_MECH, dpx[0], dpy[0], tile, facing, fy,
              mech_pose == POSE_DEAD ? SPAL_DEAD : (vis_g == G_DOWN ? SPAL_MECH_P : SPAL_MECH_G));
    } else {
        spr_hide4(SP_MECH);
    }
    /* crates moving as sprites */
    for (i = 0; i < MAX_CRATES; i++) {
        if (dvis[SLOT_CRATE + i] && (crate_spr & (1u << i)))
            spr16((uint8_t)(SP_CRATE + i * 4), dpx[SLOT_CRATE + i], dpy[SLOT_CRATE + i], SPR_CRATE, 0, 0, SPAL_CRATE);
        else
            spr_hide4((uint8_t)(SP_CRATE + i * 4));
    }
    /* drones */
    for (i = 0; i < MAX_BOTS; i++) {
        uint8_t k = (uint8_t)(SLOT_BOT + i);
        if (!dvis[k]) {
            spr_hide4((uint8_t)(SP_BOT + i * 4));
            continue;
        }
        tile = dmove[k] ? SPR_BOT_WALK : (((frame + i * 24) >> 4) & 1) ? SPR_BOT_B : SPR_BOT_A;
        spr16((uint8_t)(SP_BOT + i * 4), dpx[k], dpy[k], tile, (uint8_t)(dpx[k] > dpx[0]), fy,
              SPAL_BOT | S_PALETTE);
    }
    /* the hint */
    if (hint_shown) {
        int16_t hx = dpx[0] + 4, hy = dpy[0] + (vis_g == G_DOWN ? -10 : 18);
        uint8_t t = SPR_ARROW_R, p = SPAL_WHITE;
        if (hint_act == ACT_L) { hx = dpx[0] - 8; hy = dpy[0] + 4; p |= S_FLIPX; }
        else if (hint_act == ACT_R) { hx = dpx[0] + 16; hy = dpy[0] + 4; }
        else t = SPR_FLIP_ICON;
        if ((frame >> 3) & 1) hx += (hint_act == ACT_L) ? -1 : (hint_act == ACT_R) ? 1 : 0;
        set_sprite_tile(SP_HINT, t);
        set_sprite_prop(SP_HINT, p);
        move_sprite(SP_HINT, (uint8_t)((uint8_t)(room_x8 - scroll_x) + hx + 8),
                    (uint8_t)((uint8_t)(room_y8 - scroll_y) + hy + 16));
    } else {
        move_sprite(SP_HINT, 0, 0);
    }
}

static void place_all(const State *s)
{
    uint8_t i;
    for (i = 0; i < NOBJ; i++) {
        dvis[i] = (uint8_t)(s->o[i] != NONE);
        dmove[i] = 0;
        if (s->o[i]) {
            dpx[i] = (int16_t)(CX(s->o[i]) * 16);
            dpy[i] = (int16_t)(CY(s->o[i]) * 16);
        }
    }
    crate_spr = 0;
    render_hide = 0;
    vis_g = s->g;
}

/* ------------------------------------------------------------------ frame */

static void tick(void)
{
    draw_objects();
    wait_vbl_done();
    input_poll();
    fx_update();
    if (pose_t < 255) pose_t++;
    bg_run();
    /* buffer one action while something animates */
    if (joy_new & J_LEFT) queued = ACT_L;
    else if (joy_new & J_RIGHT) queued = ACT_R;
    else if (joy_new & J_A) queued = ACT_FLIP;
}

static void ticks(uint8_t n)
{
    while (n--) tick();
}

/* ------------------------------------------------------------------ HUD */

static void hud_draw(void)
{
    uint8_t x = 0, i;
    gfill(1, 0, 0, 20, 1, TILE_BLANK, PAL_UI);
    put_tile(1, x++, 0, st.g == G_DOWN ? UI_GRAV_DOWN : UI_GRAV_UP, st.g == G_DOWN ? PAL_PURPLE : PAL_GREEN);
    txt(1, x++, 0, "R", PAL_UI);
    txt_num(1, x, 0, room.index > 999 ? 999 : room.index, 3, PAL_UI);
    x += 4;
    if (run.mode == MODE_RUN) {
        put_tile(1, x++, 0, UI_BOLT, PAL_UI);
        txt_num(1, x, 0, run.energy, 2, run.energy < 10 ? PAL_HAZARD : PAL_UI);
    } else {
        txt(1, x, 0, "ZEN", PAL_GREEN);
        x++;
    }
    x += 3;
    if (room.npicks) {
        for (i = 0; i < room.npicks; i++)
            put_tile(1, x++, 0, UI_CELL, (st.got & (1u << i)) ? PAL_GREEN : PAL_DIM);
    }
    x = 15;
    txt_num(1, x, 0, run.moves > 99 ? 99 : run.moves, 2, run.moves > room.par ? PAL_PURPLE : PAL_UI);
    txt(1, 17, 0, "/", PAL_DIM);
    txt_num(1, 18, 0, room.par, 2, PAL_GREEN);
}

static void hud_msg(const char *s, uint8_t pal)
{
    gfill(1, 0, 0, 20, 1, TILE_BLANK, pal);
    txt(1, (uint8_t)((20 - strlen(s)) / 2), 0, s, pal);
}

/* ------------------------------------------------------------------ history */

static void hist_push(const State *s)
{
    memcpy(&hist[hist_top], s, sizeof(State));
    hist_top = (uint8_t)((hist_top + 1) % HIST_N);
    if (hist_n < HIST_N) hist_n++;
}

static uint8_t hist_pop(State *s)
{
    if (!hist_n) return 0;
    hist_top = (uint8_t)((hist_top + HIST_N - 1) % HIST_N);
    hist_n--;
    memcpy(s, &hist[hist_top], sizeof(State));
    return 1;
}

/* redraw what can differ between two states of this room */
static void redraw_diff(const State *a, const State *b)
{
    uint8_t i;
    for (i = 0; i < MAX_CRATES; i++) {
        if (a->o[SLOT_CRATE + i] != b->o[SLOT_CRATE + i]) {
            if (a->o[SLOT_CRATE + i]) render_cell(&room, b, a->o[SLOT_CRATE + i]);
            if (b->o[SLOT_CRATE + i]) render_cell(&room, b, b->o[SLOT_CRATE + i]);
        }
    }
    if (a->got != b->got) {
        for (i = 0; i < RN; i++)
            if (IS_PICK(room.cell[i])) render_cell(&room, b, i);
    }
    render_dynamic(&room, b);
}

/* ------------------------------------------------------------------ effects */

static uint8_t px_of(uint8_t c) { return (uint8_t)(CX(c) * 16 + 8); }
static uint8_t py_of(uint8_t c) { return (uint8_t)(CY(c) * 16 + 8); }

static uint8_t scr_x(int16_t x) { return (uint8_t)((uint8_t)(room_x8 - scroll_x) + x); }
static uint8_t scr_y(int16_t y) { return (uint8_t)((uint8_t)(room_y8 - scroll_y) + y); }

static void dust(uint8_t slot)
{
    /* a puff where the object meets the surface */
    int16_t y = vis_g == G_DOWN ? dpy[slot] + 14 : dpy[slot] - 2;
    part_spawn(scr_x(dpx[slot] + 2), scr_y(y), -5, vis_g ? 2 : -2, SPR_PIXEL, SPAL_WHITE, 12);
    part_spawn(scr_x(dpx[slot] + 10), scr_y(y), 5, vis_g ? 2 : -2, SPR_PIXEL, SPAL_WHITE, 12);
}

static void gravity_rain(uint8_t g)
{
    /* streaks race across the screen in the new direction of gravity */
    uint8_t i;
    for (i = 0; i < 8; i++) {
        uint8_t x = (uint8_t)(8 + (fx_rand() % 144));
        uint8_t y = (uint8_t)(g == G_DOWN ? 8 + (fx_rand() & 31) : 104 + (fx_rand() & 31));
        part_spawn(x, y, 0, g == G_DOWN ? 40 : -40, SPR_STREAK, g == G_DOWN ? SPAL_SPARK_P : SPAL_SPARK_G,
                   (uint8_t)(10 + (fx_rand() & 7)));
    }
}

static void explode(int16_t px, int16_t py, uint8_t pal)
{
    part_burst(scr_x(px + 4), scr_y(py + 4), 10, SPR_SPARK_BIG, pal, 24, 0);
    part_burst(scr_x(px + 4), scr_y(py + 4), 4, SPR_SPARK_SMALL, SPAL_WHITE, 12, 0);
}

/* ------------------------------------------------------------------ the turn */

static int16_t fall_v[NOBJ];
static int16_t fall_to[NOBJ];   /* target y (pixels) */
static uint8_t falling;          /* bit per object */

static void blocked_feedback(uint8_t act)
{
    uint8_t i;
    sfx_play(act == ACT_FLIP ? SFX_ERROR : SFX_BUMP);
    if (act == ACT_FLIP) {
        fx_shake(1);
        ticks(6);
        return;
    }
    /* lean into the wall and back */
    for (i = 0; i < 6; i++) {
        dpx[0] += (i < 3) ? (act == ACT_L ? -1 : 1) : (act == ACT_L ? 1 : -1);
        tick();
    }
}

/* returns the TR_* of the turn */
static uint8_t do_turn(uint8_t act)
{
    State ns;
    uint8_t r, i, f, k, moved_bots = 0;
    int16_t tx;

    memcpy(&ns, &st, sizeof(State));
    r = sim_turn(&room, &ns, act, &lg);
    if (r == TR_BLOCKED) {
        blocked_feedback(act);
        return r;
    }
    hint_shown = 0;
    hist_push(&st);
    run_on_move(&run);
    if (act == ACT_L) facing = 1;
    else if (act == ACT_R) facing = 0;

    /* crates that move this turn leave the background and become sprites */
    for (i = 0; i < MAX_CRATES; i++) {
        k = (uint8_t)(SLOT_CRATE + i);
        if (lg.start[k] && lg.start[k] != lg.end[k]) {
            crate_spr |= (uint8_t)(1u << i);
            render_hide |= (uint8_t)(1u << i);
            render_cell(&room, &st, lg.start[k]);
        }
    }
    for (i = SLOT_BOT; i < NOBJ; i++)
        if (lg.start[i] && lg.step[i] != lg.start[i]) moved_bots = 1;

    /* ---- phase 1: the step (or the flip) and the drones' step, 8 frames ---- */
    if (act == ACT_FLIP) {
        sfx_play(SFX_FLIP);
        mech_pose = POSE_FLIP;
        pose_t = 0;
        gravity_rain(ns.g);
        flash(3, 0);
    } else {
        mech_pose = POSE_WALK;
        pose_t = 0;
        sfx_play((lg.flags & EV_PUSH) ? SFX_PUSH : SFX_STEP);
    }
    if (moved_bots) sfx_play(SFX_BOT_STEP);
    for (f = 1; f <= 8; f++) {
        for (i = 0; i < NOBJ; i++) {
            if (!lg.start[i]) continue;
            tx = (int16_t)(CX(lg.step[i] ? lg.step[i] : lg.start[i]) * 16);
            if (i == 0 && (r == TR_WIN) && act == ACT_R && lg.end[0] == lg.start[0])
                tx = (int16_t)(RW * 16);            /* walking out of the doorway */
            if (i == 0 && r == TR_DEAD && act != ACT_FLIP && lg.died_at[0] != lg.start[0] &&
                CY(lg.died_at[0]) == CY(lg.start[0]))
                tx = (int16_t)(CX(lg.died_at[0]) * 16);
            dpx[i] = (int16_t)(CX(lg.start[i]) * 16) + (int16_t)((tx - (int16_t)(CX(lg.start[i]) * 16)) * f / 8);
            dmove[i] = (uint8_t)(tx != (int16_t)(CX(lg.start[i]) * 16));
        }
        if (act == ACT_FLIP && f == 4) {
            vis_g = ns.g;
            render_dynamic(&room, &ns);     /* the phase blocks swap */
            hud_draw();
        }
        tick();
    }
    for (i = 0; i < NOBJ; i++) dmove[i] = 0;
    if (r == TR_WIN && act == ACT_R) {
        memcpy(&st, &ns, sizeof(State));
        return r;
    }

    /* ---- phase 2: everything falls ---- */
    falling = 0;
    for (i = 0; i < NOBJ; i++) {
        uint8_t from = lg.step[i] ? lg.step[i] : lg.start[i], to;
        if (!lg.start[i] || !from) continue;
        to = lg.end[i] ? lg.end[i] : lg.died_at[i];
        if (i == 0 && r == TR_DEAD) to = lg.died_at[0] && lg.cause == DIE_ZAP ? lg.died_at[0] : lg.end[0];
        if (!to) continue;
        dpy[i] = (int16_t)(CY(from) * 16);
        fall_to[i] = (int16_t)(CY(to) * 16);
        if (i == 0 && r == TR_WIN) fall_to[0] = ns.g == G_DOWN ? (int16_t)(RH * 16 + 8) : -24;
        if (fall_to[i] != dpy[i]) {
            falling |= (uint8_t)(1u << i);
            fall_v[i] = 0;
        }
    }
    if (falling & 1) mech_pose = POSE_FALL;
    while (falling) {
        for (i = 0; i < NOBJ; i++) {
            if (!(falling & (1u << i))) continue;
            if (fall_v[i] < 8) fall_v[i]++;
            if (fall_to[i] > dpy[i]) {
                dpy[i] += fall_v[i];
                if (dpy[i] >= fall_to[i]) dpy[i] = fall_to[i];
            } else {
                dpy[i] -= fall_v[i];
                if (dpy[i] <= fall_to[i]) dpy[i] = fall_to[i];
            }
            /* a drone crushed by a falling crate goes when the crate reaches it */
            for (k = SLOT_BOT; k < NOBJ; k++) {
                if (dvis[k] && !lg.end[k] && lg.died_at[k] && lg.killer[k] == i &&
                    ((fall_to[i] > (int16_t)(CY(lg.died_at[k]) * 16)) ?
                        dpy[i] >= (int16_t)(CY(lg.died_at[k]) * 16) - 12 :
                        dpy[i] <= (int16_t)(CY(lg.died_at[k]) * 16) + 12)) {
                    dvis[k] = 0;
                    explode(dpx[k], dpy[k], SPAL_DEAD);
                    sfx_play(SFX_BOT_DIE);
                    fx_shake(2);
                }
            }
            if (dpy[i] == fall_to[i]) {
                falling &= (uint8_t)~(1u << i);
                if (i >= SLOT_BOT && !lg.end[i]) {
                    if (dvis[i]) {
                        dvis[i] = 0;
                        explode(dpx[i], dpy[i], SPAL_DEAD);
                        sfx_play(SFX_BOT_DIE);
                    }
                } else if (r != TR_WIN || i) {
                    dust(i);
                    if (fall_v[i] >= 4) {
                        if (i == 0) {
                            sfx_play(SFX_LAND);
                            mech_pose = POSE_LAND;
                            pose_t = 0;
                            fx_shake(fall_v[i] >= 8 ? 2 : 1);
                        } else if (i < SLOT_BOT) {
                            sfx_play(SFX_CRATE_LAND);
                            fx_shake(2);
                        }
                    }
                }
            }
        }
        tick();
    }
    if (mech_pose == POSE_LAND) ticks(3);
    mech_pose = POSE_IDLE;

    /* ---- phase 3: the results ---- */
    for (i = 0; i < MAX_CRATES; i++) {
        if (crate_spr & (1u << i)) {
            crate_spr &= (uint8_t)~(1u << i);
            render_hide &= (uint8_t)~(1u << i);
            if (ns.o[SLOT_CRATE + i]) render_cell(&room, &ns, ns.o[SLOT_CRATE + i]);
        }
    }
    if (lg.flags & EV_PICK) {
        for (i = 0; i < RN; i++) {
            if (IS_PICK(room.cell[i]) && (lg.picked & (1u << (room.cell[i] - T_PICK0)))) {
                render_cell(&room, &ns, i);
                part_burst(scr_x(px_of(i) - 4), scr_y(py_of(i) - 4), 8, SPR_STAR, SPAL_SPARK_G, 16, 0);
            }
        }
        sfx_play(SFX_CELL);
    }
    if ((lg.flags & EV_DOOR) || (lg.flags & EV_EXIT_OPEN) || act == ACT_FLIP || room.mechs & MBIT(M_DOOR))
        render_dynamic(&room, &ns);
    if (lg.flags & EV_DOOR) sfx_play(SFX_DOOR);
    if (lg.flags & EV_EXIT_OPEN) {
        sfx_play(SFX_EXIT_OPEN);
        flash(4, 1);
        part_burst(scr_x(px_of(room.exit) - 4), scr_y(py_of(room.exit) - 4), 12, SPR_SPARK_BIG, SPAL_SPARK_G, 20, 0);
    }
    memcpy(&st, &ns, sizeof(State));
    hud_draw();
    return r;
}

/* ------------------------------------------------------------------ death & rewind */

static void restore(const State *s)
{
    State old;
    memcpy(&old, &st, sizeof(State));
    memcpy(&st, s, sizeof(State));
    redraw_diff(&old, &st);
    place_all(&st);
    hud_draw();
}

static void death(void)
{
    uint8_t i;
    mech_pose = POSE_DEAD;
    sfx_play(lg.cause == DIE_ZAP ? SFX_ZAP : SFX_CRUSH);
    fx_shake(4);
    flash(4, 1);
    explode(dpx[0], dpy[0], SPAL_DEAD);
    hud_msg(lg.cause == DIE_ZAP ? "PLASMA BREACH" : lg.cause == DIE_CRUSH ? "HULL CRUSHED" : "DRONE CONTACT", PAL_HAZARD);
    for (i = 0; i < 40; i++) {
        if ((i & 7) == 0) explode(dpx[0], dpy[0], SPAL_DEAD);
        tick();
    }
    /* the turn is undone for free; its energy stays spent */
    if (hist_pop(&st)) {
        State s;
        memcpy(&s, &st, sizeof(State));
        memcpy(&st, &s, sizeof(State));
    }
    sfx_play(SFX_REWIND);
    music_set_rewind(1);
    render_build(&room, &st);
    render_blit_all(room_ox, room_oy);
    music_set_rewind(0);
    place_all(&st);
    mech_pose = POSE_IDLE;
    hud_draw();
}

static void rewind_one(void)
{
    State s;
    if (!hist_pop(&s)) {
        sfx_play(SFX_ERROR);
        return;
    }
    sfx_play(SFX_REWIND);
    restore(&s);
    hint_shown = 0;
}

/* ------------------------------------------------------------------ hints */

static void do_hint(void)
{
    uint8_t n = 0;
    if (!run_try_hint(&run)) {
        sfx_play(SFX_ERROR);
        hud_msg("NOT ENOUGH ENERGY", PAL_HAZARD);
        ticks(40);
        hud_draw();
        return;
    }
    sfx_play(SFX_HINT);
    bg_request_hint(&room, &st);
    hud_msg("COMPUTING...", PAL_GREEN);
    while (bg_hint_state == 1) {
        tick();
        if (++n == 0) n = 1;
    }
    bg_hint_state = 0;
    if (!bg_hint_rem) {
        hud_msg("NO WAY OUT: REWIND", PAL_HAZARD);
        ticks(50);
    } else {
        hint_act = bg_hint_act;
        hint_shown = 1;
        hud_msg("", PAL_UI);
        txt(1, 2, 0, "TURNS LEFT", PAL_GREEN);
        txt_num(1, 14, 0, bg_hint_rem, 2, PAL_UI);
        ticks(40);
    }
    hud_draw();
}

/* ------------------------------------------------------------------ map & pause */

static void map_add(int16_t rx, int16_t ry)
{
    map_rx[map_head] = rx;
    map_ry[map_head] = ry;
    map_head = (uint8_t)((map_head + 1) % MAP_N);
    if (map_n < MAP_N) map_n++;
}

static void show_map(void)
{
    uint8_t i, k;
    int16_t x, y;
    clear_win();
    move_win(7, 0);
    txt(1, 1, 0, "STATION MAP", PAL_GREEN);
    txt(1, 13, 0, "DECK", PAL_DIM);
    txt_num(1, 18, 0, (uint8_t)((room.index - 1) / DECK_ROOMS + 1), 2, PAL_UI);
    for (i = 0; i < map_n; i++) {
        k = (uint8_t)((map_head + MAP_N - 1 - i) % MAP_N);
        x = 10 + map_rx[k] - door.rx;
        y = 9 + map_ry[k] - door.ry;
        if (x < 0 || x > 19 || y < 2 || y > 16) continue;
        put_tile(1, (uint8_t)x, (uint8_t)y, i == 0 ? UI_STAR : UI_BAR_FULL, i == 0 ? PAL_GREEN : PAL_PURPLE);
    }
    txt(1, 3, 17, "B: BACK", PAL_DIM);
    gfx_hide_sprites();
    do { tick(); } while (!(joy_new & (J_B | J_START | J_A)));
    sfx_play(SFX_MENU);
}

/* returns 0 resume, 1 restart, 2 quit */
static uint8_t pause_menu(void)
{
    static const char *const items[5] = { "RESUME", "RESTART ROOM", "HINT", "STATION MAP", "QUIT RUN" };
    uint8_t sel = 0, i, result = 0xFF;
    music_set_muffle(1);
    sfx_play(SFX_SELECT);
    while (result == 0xFF) {
        clear_win();
        move_win(7, 56);
        draw_box(1, 2, 0, 16, 9, PAL_PURPLE);
        txt(1, 4, 0, " PAUSED ", PAL_GREEN);
        for (i = 0; i < 5; i++) txt(1, 5, (uint8_t)(2 + i), items[i], i == sel ? PAL_GREEN : PAL_UI);
        txt(1, 4, 8, "SEED", PAL_DIM);
        txt_hex(1, 9, 8, run.seed, PAL_UI);
        for (;;) {
            put_tile(1, 4, (uint8_t)(2 + sel), FONT_TILE('>'), PAL_GREEN);
            tick();
            if (joy_new & (J_UP | J_DOWN)) {
                put_tile(1, 4, (uint8_t)(2 + sel), TILE_BLANK, PAL_UI);
                txt(1, 5, (uint8_t)(2 + sel), items[sel], PAL_UI);
                sel = (uint8_t)((sel + ((joy_new & J_UP) ? 4 : 1)) % 5);
                txt(1, 5, (uint8_t)(2 + sel), items[sel], PAL_GREEN);
                sfx_play(SFX_MENU);
            } else if (joy_new & (J_B | J_START)) {
                result = 0;
                break;
            } else if (joy_new & J_A) {
                sfx_play(SFX_SELECT);
                if (sel == 3) {
                    show_map();
                    break;              /* redraw the menu */
                }
                result = sel == 0 ? 0 : sel == 1 ? 1 : sel == 2 ? 3 : 2;
                break;
            }
        }
    }
    clear_win();
    move_win(7, HUD_WY);
    music_set_muffle(0);
    hud_draw();
    return result;
}

/* ------------------------------------------------------------------ rooms */

static void start_state(void)
{
    state_start(&room, &st);
    hist_n = 0;
    hist_top = 0;
    run.moves = 0;
    hint_shown = 0;
    queued = 0xFF;
}

static void music_for_room(void)
{
    static const uint8_t songs[3] = { SONG_DECK_A, SONG_DECK_B, SONG_DECK_C };
    uint8_t s = run.mode == MODE_ZEN ? SONG_ZEN : songs[((room.index - 1) / DECK_ROOMS) % 3];
    if (!music_playing() || s != (uint8_t)0xFF) music_play(s);
}

static void announce_room(uint8_t deck_change)
{
    uint8_t d = (uint8_t)((room.index - 1) / DECK_ROOMS);
    if (deck_change) {
        hud_msg("", PAL_GREEN);
        txt(1, 1, 0, "DECK", PAL_DIM);
        txt_num(1, 6, 0, (uint8_t)(d + 1), 2, PAL_GREEN);
        txt(1, 9, 0, deck_names[d % 8], PAL_UI);
        sfx_play(SFX_DECK);
        ticks(70);
    } else if (room.featured != 0xFF) {
        static const char *const lessons[NUM_MECH] = {
            "NEW: PLASMA", "NEW: ENERGY CELLS", "NEW: CARGO", "NEW: HUNTER DRONES",
            "NEW: PHASE BLOCKS", "NEW: PLATES+DOORS" };
        hud_msg(lessons[room.featured], PAL_GREEN);
        sfx_play(SFX_NEWMECH);
        ticks(70);
    }
    hud_draw();
}

/* wait for the generator, with a progress readout */
static void wait_next_room(void)
{
    uint8_t n = 0;
    if (bg_ready()) return;
    hud_msg("SCANNING", PAL_GREEN);
    while (!bg_ready()) {
        tick();
        scan_frames++;
        if ((++n & 15) == 0) {
            sfx_play(SFX_SCAN);
            put_tile(1, (uint8_t)(14 + ((n >> 4) & 3)), 0, (n & 16) ? UI_BAR_FULL : UI_BAR_HALF, PAL_GREEN);
        }
    }
}

/* scroll from this room into the next one (built and waiting in the queue) */
static void transition(void)
{
    uint8_t dir = room.exit_side, nox = room_ox, noy = room_oy, done_cols = 0, steps, s;
    uint32_t written = 0;               /* bit per column / row of the new room */
    uint8_t total, i, vis0, vis1;
    int16_t mx, my;
    State ns;
    uint8_t deck_change;

    if (dir == SIDE_R) nox = (uint8_t)((room_ox + ROOM_TW) & 31);
    else if (dir == SIDE_T) noy = (uint8_t)((room_oy - ROOM_TH) & 31);
    else noy = (uint8_t)((room_oy + ROOM_TH) & 31);

    bg_take(&room, &door);
    if (run.mode == MODE_ZEN) {
        save.zen_seed = run.seed;
        memcpy(&save.zen_door, &door, sizeof(Door));
        save_write();
    }
    state_start(&room, &ns);
    render_hide = 0;
    render_build(&room, &ns);
    /* the mech's position in the new room's coordinates */
    mx = dpx[0];
    my = dpy[0];
    if (dir == SIDE_R) mx -= RW * 16;
    else if (dir == SIDE_T) my += RH * 16;
    else my -= RH * 16;
    for (i = 1; i < NOBJ; i++) dvis[i] = 0;
    total = dir == SIDE_R ? ROOM_TW : ROOM_TH;
    steps = dir == SIDE_R ? 40 : 36;        /* 4 px a frame */
    for (s = 0; s <= steps; s++) {
        /* write every new column / row that is off screen right now */
        vis0 = dir == SIDE_R ? (uint8_t)(scroll_x >> 3) : (uint8_t)(scroll_y >> 3);
        vis1 = (uint8_t)(vis0 + (dir == SIDE_R ? 20 : 18));
        for (i = 0; i < total && done_cols < 3; i++) {
            uint8_t m, d;
            if (written & ((uint32_t)1 << i)) continue;
            m = (uint8_t)(((dir == SIDE_R ? nox : noy) + i) & 31);
            d = (uint8_t)((m - vis0) & 31);
            if (d <= (uint8_t)(vis1 - vis0)) continue;     /* still on screen */
            if (dir == SIDE_R) render_blit_col(i, nox, noy);
            else render_blit_row(i, nox, noy);
            written |= (uint32_t)1 << i;
            done_cols++;
        }
        done_cols = 0;
        if (s == steps) break;
        if (dir == SIDE_R) scroll_x += 4;
        else if (dir == SIDE_T) scroll_y -= 4;
        else scroll_y += 4;
        /* the mech keeps moving with the camera, into the doorway */
        if (dir == SIDE_R && dpx[0] < RW * 16 + 16) dpx[0] += 1;
        tick();
    }
    /* anything not yet written (it was on screen the whole time) */
    for (i = 0; i < total; i++)
        if (!(written & ((uint32_t)1 << i))) {
            if (dir == SIDE_R) render_blit_col(i, nox, noy);
            else render_blit_row(i, nox, noy);
        }
    room_ox = nox;
    room_oy = noy;
    room_x8 = (uint8_t)(room_ox * 8);
    room_y8 = (uint8_t)(room_oy * 8);
    scroll_x = room_x8;
    scroll_y = room_y8;

    deck_change = (uint8_t)(room.index > 1 && (room.index - 1) % DECK_ROOMS == 0);
    if (deck_change) {
        pal_set_deck((uint8_t)((room.index - 1) / DECK_ROOMS));
        flash(6, 1);
        music_for_room();
    }
    map_add(door.rx, door.ry);

    /* the mech arrives: from the side it walks in, from above / below it drops in */
    memcpy(&st, &ns, sizeof(State));
    hist_n = 0;
    hist_top = 0;
    run.moves = 0;
    hint_shown = 0;
    vis_g = room.g0;
    dvis[0] = 1;
    dpx[0] = (int16_t)(CX(room.entry) * 16);
    dpy[0] = (int16_t)(CY(room.entry) * 16);
    if (room.entry_side == SIDE_L && mx < 0) dpx[0] = mx;
    if (room.entry_side != SIDE_L) dpy[0] = my;
    for (i = 1; i < NOBJ; i++) {
        dvis[i] = (uint8_t)(st.o[i] != NONE);
        if (st.o[i]) {
            dpx[i] = (int16_t)(CX(st.o[i]) * 16);
            dpy[i] = (int16_t)(CY(st.o[i]) * 16);
        }
    }
    /* walk / drop to the start position */
    mech_pose = POSE_WALK;
    while (dpx[0] < (int16_t)(CX(st.o[0]) * 16)) { dpx[0] += 2; tick(); }
    {
        int16_t ty = (int16_t)(CY(st.o[0]) * 16), v = 2;
        mech_pose = POSE_FALL;
        while (dpy[0] != ty) {
            if (v < 8) v++;
            if (dpy[0] < ty) { dpy[0] += v; if (dpy[0] > ty) dpy[0] = ty; }
            else { dpy[0] -= v; if (dpy[0] < ty) dpy[0] = ty; }
            tick();
        }
    }
    mech_pose = POSE_IDLE;
    place_all(&st);
    queued = 0xFF;
    /* gravity realigns in a side airlock */
    hud_draw();
    announce_room(deck_change);
}

static void enter_first_room(const Door *d)
{
    bg_reset(run.seed, d);
    clear_win();
    move_win(7, HUD_WY);
    SHOW_WIN;
    hud_msg("BOOTING", PAL_GREEN);
    wait_next_room();
    bg_take(&room, &door);
    start_state();
    room_ox = room_oy = 0;
    room_x8 = room_y8 = 0;
    scroll_x = scroll_y = 0;
    pal_set_deck((uint8_t)((room.index - 1) / DECK_ROOMS));
    render_hide = 0;
    render_build(&room, &st);
    render_blit_all(0, 0);
    place_all(&st);
    map_n = map_head = 0;
    map_add(door.rx, door.ry);
    fade_to(0);
    music_for_room();
    hud_draw();
    announce_room(0);
}

/* ------------------------------------------------------------------ the loop */

static uint8_t clear_room(void)
{
    uint8_t gain, grade;
    grade = run_grade(run.moves, room.par);
    gain = run_on_win(&run, room.par);
    sfx_play(SFX_EXIT);
    if (grade == GRADE_BEST) {
        save.total_opt++;
        sfx_play(SFX_OPTIMAL);
        hud_msg("", PAL_GREEN);
        txt(1, 2, 0, "OPTIMAL", PAL_GREEN);
        part_burst(80, 70, 12, SPR_STAR, SPAL_SPARK_G, 28, 0);
    } else {
        hud_msg("", PAL_UI);
        txt(1, 2, 0, grade == GRADE_GOOD ? "ALIGNED" : "CLEARED", PAL_UI);
    }
    if (run.mode == MODE_RUN) {
        put_tile(1, 12, 0, UI_BOLT, PAL_UI);
        txt(1, 13, 0, "+", PAL_GREEN);
        txt_num(1, 14, 0, gain, 2, PAL_GREEN);
    }
    save.total_rooms++;
    if (run.mode == MODE_RUN) {
        if (run.room > save.run_best_room) {
            save.run_best_room = run.room;
            save.run_best_opt = run.bests;
        }
    }
    ticks(30);
    wait_next_room();
    save_write();
    transition();
    return 0;
}

uint8_t play_run(const Door *d) BANKED
{
    uint8_t act, r, held = 0, p;

    gfx_load_play_tiles();
    gfx_hide_sprites();
    part_clear();
    enter_first_room(d);

    for (;;) {
        play_idle = 1;
        tick();
        if (run.mode == MODE_RUN && run.energy == 0) {
            /* out of energy */
            hud_msg("POWER DEPLETED", PAL_HAZARD);
            music_stop();
            ticks(90);
            return PLAY_OVER;
        }
        if (joy_new & J_START) {
            play_idle = 0;
            p = pause_menu();
            if (p == 1) {
                State s0;
                state_start(&room, &s0);
                restore(&s0);
                hist_n = 0;
                hist_top = 0;
                sfx_play(SFX_REWIND);
            } else if (p == 2) {
                music_stop();
                return PLAY_QUIT;
            } else if (p == 3) {
                do_hint();
            }
            continue;
        }
        if (joy_new & J_SELECT) {
            do_hint();
            continue;
        }
        if (joy & J_B) {
            /* hold B: rewind, one turn every 8 frames */
            if (!rewinding || ++held >= 8) {
                if (!rewinding) music_set_rewind(1);
                rewinding = 1;
                held = 0;
                rewind_one();
            }
            continue;
        }
        if (rewinding) {
            rewinding = 0;
            music_set_rewind(0);
        }
        act = 0xFF;
        if (queued != 0xFF) {
            act = queued;
            queued = 0xFF;
        } else if (joy & J_LEFT) act = ACT_L;
        else if (joy & J_RIGHT) act = ACT_R;
        else if (joy_new & J_A) act = ACT_FLIP;
        if (act == 0xFF) continue;
        play_idle = 0;
        r = do_turn(act);
        if (r == TR_DEAD) {
            death();
        } else if (r == TR_WIN) {
            clear_room();
        }
    }
}
