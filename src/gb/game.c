/* ALIGNMENT - the title screen, mode menu, seed entry and game over. */
#pragma bank 255
#include <gb/gb.h>
#include <string.h>
#include "gfx.h"
#include "assets.h"
#include "play.h"
#include "bgjob.h"
#include "sound.h"
#include "save.h"

BANKREF_EXTERN(assets)

static uint16_t seed_from_time(void)
{
    return (uint16_t)(((uint16_t)DIV_REG << 8) ^ frame ^ ((uint16_t)fx_rand() << 4) ^ fx_rand());
}

/* the title's little show: the mech flips gravity in the corner, streaks fall */
static uint8_t t_g;
static int16_t t_y;
static void title_fx(uint8_t t)
{
    uint8_t q, tile = (t >> 5) & 1 ? SPR_MECH_IDLE2 : SPR_MECH_IDLE, fy = t_g;
    int16_t ty = t_g ? 96 : 120;
    if ((t & 127) == 0) {
        t_g ^= 1;
        sfx_play(SFX_FLIP);
    }
    if (t_y < ty) { t_y += 4; if (t_y > ty) t_y = ty; tile = SPR_MECH_FALL; }
    if (t_y > ty) { t_y -= 4; if (t_y < ty) t_y = ty; tile = SPR_MECH_FALL; }
    for (q = 0; q < 4; q++) {
        uint8_t col = q & 1, row = q >> 1, sr = fy ? 1 - row : row;
        set_sprite_tile(SP_MECH + q, (uint8_t)(tile + sr * 2 + col));
        set_sprite_prop(SP_MECH + q, (uint8_t)((t_g ? SPAL_MECH_G : SPAL_MECH_P) | (fy ? S_FLIPY : 0)));
        move_sprite(SP_MECH + q, (uint8_t)(136 + col * 8 + 8), (uint8_t)(t_y + row * 8 + 16));
    }
    if ((t & 7) == 0) {
        uint8_t x = (uint8_t)(fx_rand() % 160);
        part_spawn(x, t_g ? 140 : 60, 0, t_g ? -24 : 24, SPR_STREAK, t_g ? SPAL_SPARK_G : SPAL_SPARK_P, 20);
    }
}

static void title_screen_draw(void)
{
    clear_bkg();
    gfx_load_title_tiles();
    gfx_draw_logo(1);
    txt(0, 2, 8, "GRAVITY  STATION", PAL_UI);
    if (save.run_best_room) {
        txt(0, 1, 16, "BEST ROOM", PAL_DIM);
        txt_num(0, 11, 16, save.run_best_room, 3, PAL_GREEN);
        put_tile(0, 15, 16, UI_STAR, PAL_GREEN);
        txt_num(0, 16, 16, save.total_opt, 3, PAL_UI);
    }
}

/* returns 0 RUN, 1 ZEN, 2 SEED */
static uint8_t title(void)
{
    static const char *const items[3] = { "NEW RUN", "ZEN WALK", "ENTER SEED" };
    uint8_t sel = 0, i, t = 0, started = 0;
    scroll_x = scroll_y = 0;
    HIDE_WIN;
    gfx_hide_sprites();
    part_clear();
    title_screen_draw();
    pal_set_deck(0);
    fade_to(0);
    music_play(SONG_TITLE);
    t_g = 0;
    t_y = 120;
    for (;;) {
        wait_vbl_done();
        input_poll();
        fx_update();
        title_fx(t++);
        if (!started) {
            txt(0, 5, 12, (t & 32) ? "PRESS START" : "           ", PAL_GREEN);
            if (joy_new & (J_START | J_A)) {
                started = 1;
                sfx_play(SFX_SELECT);
                gfill(0, 0, 12, 20, 1, TILE_BLANK, 0);
                for (i = 0; i < 3; i++) txt(0, 6, (uint8_t)(11 + i), items[i], PAL_UI);
            }
            continue;
        }
        for (i = 0; i < 3; i++) {
            put_tile(0, 4, (uint8_t)(11 + i), i == sel ? FONT_TILE('>') : TILE_BLANK, PAL_GREEN);
            txt(0, 6, (uint8_t)(11 + i), items[i], i == sel ? PAL_GREEN : PAL_UI);
        }
        if (joy_new & J_UP) { sel = (uint8_t)((sel + 2) % 3); sfx_play(SFX_MENU); }
        if (joy_new & J_DOWN) { sel = (uint8_t)((sel + 1) % 3); sfx_play(SFX_MENU); }
        if (joy_new & (J_A | J_START)) {
            sfx_play(SFX_SELECT);
            return sel;
        }
        if (joy_new & J_B) {
            started = 0;
            gfill(0, 0, 11, 20, 3, TILE_BLANK, 0);
        }
    }
}

static uint8_t seed_entry(uint16_t *seed)
{
    uint8_t pos = 0, d, i;
    uint16_t v = *seed;
    gfill(0, 0, 10, 20, 5, TILE_BLANK, 0);
    txt(0, 5, 10, "ENTER SEED", PAL_GREEN);
    txt(0, 2, 14, "A: GO  B: BACK", PAL_DIM);
    for (;;) {
        txt_hex(0, 8, 12, v, PAL_UI);
        for (i = 0; i < 4; i++)
            put_tile(0, (uint8_t)(8 + i), 13, i == pos ? FONT_TILE('^') : TILE_BLANK, PAL_GREEN);
        wait_vbl_done();
        input_poll();
        fx_update();
        d = (uint8_t)(12 - pos * 4);
        if (joy_new & J_LEFT && pos) { pos--; sfx_play(SFX_MENU); }
        if (joy_new & J_RIGHT && pos < 3) { pos++; sfx_play(SFX_MENU); }
        if (joy_new & J_UP) { v = (uint16_t)((v & ~(0xFu << d)) | ((((v >> d) + 1) & 15u) << d)); sfx_play(SFX_MENU); }
        if (joy_new & J_DOWN) { v = (uint16_t)((v & ~(0xFu << d)) | ((((v >> d) + 15) & 15u) << d)); sfx_play(SFX_MENU); }
        if (joy_new & J_SELECT) { v = seed_from_time(); sfx_play(SFX_MENU); }
        if (joy_new & (J_A | J_START)) { *seed = v; sfx_play(SFX_SELECT); return 1; }
        if (joy_new & J_B) return 0;
    }
}

static void game_over(void)
{
    uint8_t t;
    gfx_hide_sprites();
    part_clear();
    fade_to(4);
    HIDE_WIN;
    scroll_x = scroll_y = 0;
    clear_bkg();
    fade_to(0);
    music_play(SONG_GAMEOVER);
    txt(0, 4, 3, "RUN COMPLETE", PAL_HAZARD);
    txt(0, 3, 6, "ROOMS", PAL_DIM);
    txt_num(0, 13, 6, run.cleared, 4, PAL_UI);
    txt(0, 3, 8, "OPTIMAL", PAL_DIM);
    txt_num(0, 13, 8, run.bests, 4, PAL_GREEN);
    txt(0, 3, 10, "BEST STREAK", PAL_DIM);
    txt_num(0, 13, 10, run.best_streak, 4, PAL_UI);
    txt(0, 3, 12, "SEED", PAL_DIM);
    txt_hex(0, 13, 12, run.seed, PAL_PURPLE);
    if (run.room >= save.run_best_room) txt(0, 3, 14, "NEW RECORD!", PAL_GREEN);
    save_write();
    for (t = 0; ; t++) {
        wait_vbl_done();
        input_poll();
        fx_update();
        if ((t & 31) == 0) part_burst((uint8_t)(20 + fx_rand() % 120), (uint8_t)(20 + fx_rand() % 100), 6, SPR_STAR, SPAL_SPARK_P, 12, 0);
        if (t > 60 && (joy_new & (J_A | J_START | J_B))) break;
    }
    fade_to(4);
}

void game_main(void) BANKED
{
    uint8_t m;
    uint16_t seed = 0;
    Door d;
    save_load();
    bg_init();
    for (;;) {
        m = title();
        if (m == 2) {
            seed = seed_from_time();
            if (!seed_entry(&seed)) continue;
        } else if (m == 0) {
            seed = seed_from_time();
        }
        fade_to(4);
        gfx_hide_sprites();
        part_clear();
        clear_bkg();
        if (m == 1) {
            /* ZEN: the station walk resumes where it was left */
            if (!save.zen_seed) {
                save.zen_seed = seed_from_time() | 1;
                gen_first_door(&save.zen_door);
                save_write();
            }
            run_start(&run, MODE_ZEN, save.zen_seed, save.zen_door.index);
            memcpy(&d, &save.zen_door, sizeof(Door));
        } else {
            run_start(&run, MODE_RUN, seed, 1);
            gen_first_door(&d);
        }
        if (play_run(&d) == PLAY_OVER) game_over();
        else fade_to(4);
    }
}
