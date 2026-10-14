/* ALIGNMENT - playing a stage: input, physics, the camera and the streaming map,
 * enemies, effects, the HUD, death and the checkpoint, the stage card and the tally. */
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

Run run;
Stage stage;                /* (globals: the ROM tests read them) */
Mech mech;
uint16_t stage_t;           /* the stage clock: enemies and lasers run on it */
uint8_t play_state;         /* PS_*: what the game is doing */
uint16_t play_lag;          /* frames the loop missed (the ROM tests check it stays 0) */
uint8_t play_ly;            /* the scanline the last frame's work ended on (tests) */
static uint8_t last_frame;
uint8_t dbg_ly[8];
uint8_t dbg_f[8];           /* frame counter at the same points */
uint8_t dbg_q[4];          /* scanline after each part of the frame (profiling) */

enum { PS_CARD = 0, PS_PLAY, PS_DEAD, PS_CLEAR, PS_PAUSE };

static uint8_t spawn_x, spawn_y;    /* respawn cell */
static uint8_t checked;             /* the checkpoint has been reached */
static uint8_t col_lo, col_hi;      /* stage columns currently in the BG ring */
static uint8_t pose, pose_t;        /* mech animation */
static uint8_t hud_cells, hud_lives, hud_bar, hud_g;
static uint8_t laser_was;
static uint8_t hud_msg_t;           /* a message is up in the HUD for this many frames */
static uint8_t mech_hidden;
static uint8_t demo_jheld;       /* (demo input: B held at the end of the last move) */

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
    "HOVER DRONES", "LASER GATES", "CRUSHERS", "SHAFTS"
};

/* ------------------------------------------------------------------ helpers */

static uint16_t mech_px(void) { return (uint16_t)(mech.x >> 4); }
static uint8_t mech_py(void) { return (uint8_t)(mech.y >> 4); }

static void spr16(uint8_t slot, int16_t sx, int16_t sy, uint8_t tile, uint8_t fx, uint8_t fy, uint8_t prop)
{
    /* straight into the shadow OAM: four 8x8 sprites, mirrored as needed */
    OAM_item_t *o = &shadow_OAM[slot];
    uint8_t x0, x1, y0, y1, t0, t1, t2, t3;
    if (sx < -15 || sx > 167 || sy < -15 || sy > 151) {
        o[0].y = o[1].y = o[2].y = o[3].y = 0;
        return;
    }
    if (fx) prop |= S_FLIPX;
    if (fy) prop |= S_FLIPY;
    x0 = (uint8_t)(sx + 8);
    x1 = (uint8_t)(sx + 16);
    y0 = (uint8_t)(sy + 16);
    y1 = (uint8_t)(sy + 24);
    /* tiles TL TR BL BR, swapped by the flips */
    t0 = tile; t1 = (uint8_t)(tile + 1); t2 = (uint8_t)(tile + 2); t3 = (uint8_t)(tile + 3);
    if (fx) { uint8_t k = t0; t0 = t1; t1 = k; k = t2; t2 = t3; t3 = k; }
    if (fy) { uint8_t k = t0; t0 = t2; t2 = k; k = t1; t1 = t3; t3 = k; }
    o->y = y0; o->x = x0; o->tile = t0; o->prop = prop; o++;
    o->y = y0; o->x = x1; o->tile = t1; o->prop = prop; o++;
    o->y = y1; o->x = x0; o->tile = t2; o->prop = prop; o++;
    o->y = y1; o->x = x1; o->tile = t3; o->prop = prop;
}

static void hide4(uint8_t slot)
{
    OAM_item_t *o = &shadow_OAM[slot];
    o[0].y = o[1].y = o[2].y = o[3].y = 0;
}

/* ------------------------------------------------------------------ HUD */

static void hud_full(void)
{
    gfill(1, 0, 0, 20, 1, TILE_BLANK, PAL_UI);
    txt(1, 1, 0, "S", PAL_DIM);
    txt_num(1, 2, 0, run.stage > 99 ? 99 : run.stage, 2, PAL_UI);
    put_tile(1, 5, 0, UI_BOLT, PAL_GREEN);
    put_tile(1, 9, 0, UI_CELL, PAL_GREEN);
    txt(1, 12, 0, "/", PAL_DIM);
    txt_num(1, 13, 0, stage.ncells, 2, PAL_DIM);
    hud_cells = hud_lives = hud_bar = hud_g = 0xFF;
}

static void hud_update(void)
{
    /* one item per frame at most: each is a few STAT-safe VRAM writes */
    uint8_t b, i, t;
    if (hud_g != mech.g) {
        hud_g = mech.g;
        put_tile(1, 0, 0, mech.g ? UI_GRAV_UP : UI_GRAV_DOWN, mech.g ? PAL_GREEN : PAL_PURPLE);
        return;
    }
    if (hud_lives != run.lives) {
        hud_lives = run.lives;
        if (run.mode == MODE_ZEN) txt(1, 6, 0, "ZN", PAL_GREEN);
        else txt_num(1, 6, 0, run.lives, 2, run.lives <= 1 ? PAL_HAZARD : PAL_UI);
        return;
    }
    if (hud_cells != run.stage_cells) {
        hud_cells = run.stage_cells;
        txt_num(1, 10, 0, run.stage_cells, 2, run.stage_cells >= stage.ncells ? PAL_GREEN : PAL_UI);
        return;
    }
    if (frame & 7) return;
    /* progress through the stage: 4 tiles, half steps */
    b = (uint8_t)(((uint16_t)(mech_px() >> 4) * 8) / stage.w);
    if (b != hud_bar) {
        uint8_t row[4], pal[4];
        hud_bar = b;
        for (i = 0; i < 4; i++) {
            t = b >= (uint8_t)(i * 2 + 2) ? UI_BAR_FULL : b == (uint8_t)(i * 2 + 1) ? UI_BAR_HALF : UI_BAR_EMPTY;
            row[i] = t;
            pal[i] = PAL_PURPLE;
        }
        if (is_cgb) {
            VBK_REG = 1;
            set_win_tiles(16, 0, 4, 1, pal);
            VBK_REG = 0;
        }
        set_win_tiles(16, 0, 4, 1, row);
    }
}

static void hud_msg(const char *s, uint8_t pal)
{
    gfill(1, 0, 0, 20, 1, TILE_BLANK, pal);
    txt(1, (uint8_t)((20 - strlen(s)) / 2), 0, s, pal);
    hud_cells = hud_lives = hud_bar = hud_g = 0xFF;
}

/* ------------------------------------------------------------------ camera & map */

static uint16_t cam_target_x(void)
{
    int16_t x = (int16_t)mech_px() - 72 + (mech.facing ? -12 : 12);
    int16_t maxx = (int16_t)((uint16_t)stage.w * 16 - 160);
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
    uint8_t tile;
    int16_t sx = (int16_t)(mech_px() - cam_x) - shake_x;
    int16_t sy = (int16_t)mech_py() - (int16_t)cam_y - shake_y;
    if (mech_hidden) {
        hide4(SP_MECH);
        return;
    }
    switch (pose) {
    case POSE_RUN: tile = ((pose_t >> 3) & 1) ? SPR_MECH_WALK2 : SPR_MECH_WALK1; break;
    case POSE_AIR: tile = SPR_MECH_FALL; break;
    case POSE_FLIP: tile = SPR_MECH_FLIP; break;
    case POSE_LAND: tile = SPR_MECH_LAND; break;
    case POSE_DEAD: tile = SPR_MECH_DEAD; break;
    default: tile = ((frame >> 5) & 1) ? SPR_MECH_IDLE2 : SPR_MECH_IDLE; break;
    }
    spr16(SP_MECH, sx, sy, tile, mech.facing, mech.g == G_UP,
          pose == POSE_DEAD ? SPAL_DEAD : (mech.g == G_DOWN ? SPAL_MECH_P : SPAL_MECH_G));
}

static void draw_enemies(void)
{
    uint8_t i, n = 0, len, slot, k, rods = 0;
    uint16_t ex;
    uint8_t ey;
    int16_t sx, sy;
    for (i = 0; i < stage.nenemies && n < NUM_ESPR; i++) {
        const Enemy *e = &stage.enemy[i];
        if ((uint16_t)(e->x1 * 16 + 16) < cam_x || (uint16_t)(e->x0 * 16) > cam_x + 160) continue;
        enemy_pos(e, stage_t, &ex, &ey, &len);
        sx = (int16_t)(ex - cam_x) - shake_x;
        sy = (int16_t)ey - (int16_t)cam_y - shake_y;
        slot = (uint8_t)(SP_ENEMY + n * 4);
        if (e->kind == E_CRUSHER) {
            /* the piston head, and its rod above it */
            spr16(slot, sx, (int16_t)(sy + len), SPR_CRATE, 0, 0, SPAL_CRATE);
            for (k = 0; k < NUM_ROD && rods < NUM_ROD; k++) {
                int16_t ry = (int16_t)(sy + len - 8 - k * 16);
                if (ry < sy - 8) break;
                if (ry < -8 || ry > 151) continue;
                set_sprite_tile((uint8_t)(SP_ROD + rods), SPR_STREAK);
                set_sprite_prop((uint8_t)(SP_ROD + rods), SPAL_CRATE);
                move_sprite((uint8_t)(SP_ROD + rods), (uint8_t)(sx + 4 + 8), (uint8_t)(ry + 16));
                rods++;
            }
        } else {
            uint8_t t = e->kind == E_HOVER ? ((frame >> 2) & 1 ? SPR_BOT_A : SPR_BOT_B)
                                           : ((frame >> 3) & 1 ? SPR_BOT_WALK : SPR_BOT_A);
            spr16(slot, sx, sy, t, (uint8_t)(ex > mech_px()), e->kind == E_CRAWLER && e->y1,
                  SPAL_BOT | S_PALETTE);
        }
        n++;
    }
    for (; n < NUM_ESPR; n++) hide4((uint8_t)(SP_ENEMY + n * 4));
    for (; rods < NUM_ROD; rods++) move_sprite((uint8_t)(SP_ROD + rods), 0, 0);
}

/* ------------------------------------------------------------------ frame */

static void frame_end(void)
{
    draw_mech();
    dbg_ly[5] = LY_REG; dbg_f[5] = frame;
    draw_enemies();
    dbg_ly[6] = LY_REG; dbg_f[6] = frame;
    play_ly = LY_REG;
    wait_vbl_done();
    /* a frame of play must take exactly one frame */
    if (play_state == PS_PLAY && (uint8_t)(frame - last_frame) != 1) play_lag++;
    last_frame = frame;
    dbg_ly[7] = LY_REG; dbg_f[7] = frame;
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
    uint8_t d = (uint8_t)(stage.deck % 8);
    play_state = PS_CARD;
    gfx_hide_sprites();
    part_clear();
    HIDE_WIN;
    move_win(7, 0);
    clear_win();
    SHOW_WIN;
    txt(1, 5, 5, "STAGE", PAL_DIM);
    txt_num(1, 11, 5, run.stage > 999 ? 999 : run.stage, 3, PAL_UI);
    txt(1, 4, 7, "DECK", PAL_DIM);
    txt_num(1, 9, 7, (uint8_t)(stage.deck + 1), 2, PAL_GREEN);
    txt(1, 12, 7, deck_names[d], PAL_PURPLE);
    if (run.stage == 1) {
        txt(1, 3, 11, "A: FLIP GRAVITY", PAL_GREEN);
        txt(1, 3, 12, "B: JUMP", PAL_UI);
    } else if (stage.featured != 0xFF) {
        txt(1, 4, 11, "NEW HAZARD:", PAL_HAZARD);
        txt(1, (uint8_t)((20 - strlen(mech_names[stage.featured])) / 2), 12, mech_names[stage.featured], PAL_UI);
    }
    txt(1, 5, 15, "SEED", PAL_DIM);
    txt_hex(1, 10, 15, run.seed, PAL_DIM);
    fade_to(0);
}

static void music_for_stage(void)
{
    static const uint8_t songs[3] = { SONG_DECK_A, SONG_DECK_B, SONG_DECK_C };
    music_play(run.mode == MODE_ZEN ? SONG_ZEN : songs[stage.deck % 3]);
}

/* ------------------------------------------------------------------ spawning */

static void spawn(void)
{
    uint8_t f;
    mech_spawn(&mech, spawn_x, spawn_y, G_DOWN);
    /* settle onto the floor */
    for (f = 0; f < 24; f++) mech_step(&mech, &stage, 0, stage_t);
    pose = POSE_IDLE;
    mech_hidden = 0;
    gfx_phase(G_DOWN);
    camera(1);
}

static void stage_begin(void)
{
    uint16_t t0;
    fade_to(4);
    stage_card();
    t0 = frame;
    stream_on = 0;
    stage_gen(&stage, run.seed, run.stage);
    render_prebuild(&stage);
    pal_set_deck(stage.deck);
    /* keep the card up a moment */
    while ((uint8_t)(frame - t0) < 100) {
        wait_vbl_done();
        input_poll();
        if (joy_new & (J_A | J_START)) break;
    }
    fade_to(4);
    HIDE_WIN;
    checked = 0;
    render_checked = 0;
    spawn_x = stage.start_x;
    spawn_y = stage.start_y;
    stage_t = 0;
    demo_jheld = 0;
    run.stage_cells = 0;
    run.stage_time = 0;
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
    part_spawn((uint16_t)(mech_px() + 4), y, -6, mech.g ? 2 : -2, SPR_PIXEL, SPAL_WHITE, 12);
    part_spawn((uint16_t)(mech_px() + 11), y, 6, mech.g ? 2 : -2, SPR_PIXEL, SPAL_WHITE, 12);
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
    dbg_q[0] = LY_REG;
    sfx_play(SFX_FLIP);
    flash(3, 0);
    dbg_q[1] = LY_REG;
    gfx_phase(mech.g);
    dbg_q[2] = LY_REG;
    for (i = is_cgb ? 0 : 3; i < 6; i++) {
        uint16_t x = (uint16_t)(cam_x + 8 + (fx_rand() & 127) + (fx_rand() & 15));
        uint8_t y = (uint8_t)(cam_y + (mech.g ? 100 + (fx_rand() & 31) : 8 + (fx_rand() & 31)));
        part_spawn(x, y, 0, mech.g ? -40 : 40, SPR_STREAK, mech.g ? SPAL_SPARK_G : SPAL_SPARK_P,
                   (uint8_t)(10 + (fx_rand() & 7)));
    }
    dbg_q[3] = LY_REG;
}

static void cell_fx(void)
{
    render_cell(&stage, ev_cx, ev_cy);
    part_burst((uint16_t)(ev_cx * 16 + 4), (uint8_t)(ev_cy * 16 + 4), 6, SPR_STAR, SPAL_SPARK_G, 8, 0);
    if (run_cell(&run)) {
        sfx_play(SFX_NEWMECH);
        hud_msg("EXTRA LIFE", PAL_GREEN);
        hud_msg_t = 60;
    } else {
        sfx_play(SFX_CELL);
    }
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
    part_burst((uint16_t)(stage.check_x * 16 + 4), (uint8_t)(stage.check_y * 16), 10, SPR_SPARK_BIG, SPAL_SPARK_G, 10, -1);
    hud_msg("CHECKPOINT", PAL_GREEN);
    hud_msg_t = 60;
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
            mech.dead == DIE_CRUSHER ? "HULL CRUSHED" : "DRONE CONTACT", PAL_HAZARD);
    for (i = 0; i < 50; i++) {
        if ((i & 7) == 0)
            part_burst((uint16_t)(mech_px() + 4), (uint8_t)(mech_py() + 4), 5, SPR_SPARK_BIG, SPAL_DEAD, 12, 0);
        if (i == 20) mech_hidden = 1;      /* (the wreck vanishes) */
        frames(1);
    }
    music_set_rewind(0);
    over = run_death(&run);
    if (over) return 1;
    fade_to(4);
    part_clear();
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
    if (run.mode == MODE_RUN) {
        if (run.stage > save.best_stage) save.best_stage = run.stage;
        if (run.score > save.best_score) save.best_score = run.score;
    } else {
        save.zen_seed = run.seed;
        save.zen_stage = run.stage;
    }
    stream_on = 0;
    save_write();
    music_play(SONG_GAMEOVER);  /* (the short sting doubles as a fanfare) */
    sfx_play(all ? SFX_OPTIMAL : SFX_SELECT);
    HIDE_WIN;
    move_win(7, 32);
    clear_win();
    draw_box(1, 2, 0, 16, 9, PAL_PURPLE);
    txt(1, 4, 1, "STAGE CLEAR", PAL_GREEN);
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
    for (i = 0; i < 150; i++) {
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
    txt(1, 6, 3, "QUIT RUN", PAL_UI);
    txt(1, 5, 5, "SEED", PAL_DIM);
    txt_hex(1, 10, 5, run.seed, PAL_UI);
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
 * bot's moves here. */
#define DEMO_MAX 768
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
    if (k >= demo_len) return 0;
    a = demo_buf[k];
    in = demo_acts[a][0];
    press = (stage_t & 3) ? 0 : demo_acts[a][1];
    if ((press & IN_JUMP_NEW) && demo_jheld) press &= (uint8_t)~IN_JUMP_NEW;
    if ((stage_t & 3) == 3) demo_jheld = (uint8_t)((in & IN_JUMP) != 0);
    return (uint8_t)(in | press);
}

static uint8_t input_bits(void)
{
    if (demo_on) return demo_bits();
    {
    uint8_t j = joy, n = joy_new, in = 0;
    if (j & J_LEFT) in = IN_L;
    else if (j & J_RIGHT) in = IN_R;
    if (j & J_B) in |= IN_JUMP;
    if (n & J_B) in |= IN_JUMP_NEW;
    if (n & J_A) in |= IN_FLIP_NEW;
    return in;
    }
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
        dbg_ly[0] = LY_REG; dbg_f[0] = frame;
        /* physics */
        stage_t++;
        if (run.stage_time < 65535u) run.stage_time++;
        ev = mech_step(&mech, &stage, in, stage_t);
        dbg_ly[1] = LY_REG; dbg_f[1] = frame;
        if (ev & EV_FLIP) {
            flip_fx();
            pose = POSE_FLIP;
            pose_t = 0;
        }
        if (ev & EV_NOFLIP) sfx_play(SFX_ERROR);
        if (ev & EV_JUMP) sfx_play(SFX_BUMP);
        if (ev & EV_LAND) {
            land_fx();
            pose = POSE_LAND;
            pose_t = 0;
        } else if (ev & EV_STEP) {
            sfx_play(SFX_STEP);
        }
        if (ev & EV_CELL) cell_fx();
        if (ev & EV_CHECK) check_fx();
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
        dbg_ly[2] = LY_REG; dbg_f[2] = frame;
        camera(0);
        stream();
        dbg_ly[3] = LY_REG; dbg_f[3] = frame;
        if (hud_msg_t) {
            if (!--hud_msg_t) hud_full();
        } else {
            hud_update();
        }
        dbg_ly[4] = LY_REG; dbg_f[4] = frame;
        frame_end();
    }
}
