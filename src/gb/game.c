/* ALIGNMENT - the title screen, mode menu, seed entry and game over. */
#pragma bank 255
#include <gb/gb.h>
#include <string.h>
#include "gfx.h"
#include "assets.h"
#include "play.h"
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

#define ENDLESS_AT 5        /* the campaign sector that opens ENDLESS (after clearing 4) */

/* the ROM tests start a mode directly: they poke these while the title is up */
uint8_t dbg_mode;           /* 1 campaign, 2 endless */
uint16_t dbg_stage, dbg_seed;

static uint8_t endless_open(void)
{
    return (uint8_t)(save.sector >= ENDLESS_AT || save.campaign_done);
}

static void title_screen_draw(void)
{
    clear_bkg();
    gfx_load_title_tiles();
    gfx_draw_logo(1);
    txt(0, 2, 8, "GRAVITY  STATION", PAL_UI);
    if (save.best_score) {
        txt(0, 1, 16, "HI", PAL_DIM);
        txt_num(0, 4, 16, (uint16_t)(save.best_score / 100u), 5, PAL_GREEN);
        txt(0, 9, 16, "00", PAL_GREEN);
    }
}

static uint8_t sel_sector;

static void menu_draw(uint8_t sel)
{
    uint8_t i;
    for (i = 0; i < 3; i++)
        put_tile(0, 2, (uint8_t)(11 + i), i == sel ? FONT_TILE('>') : TILE_BLANK, PAL_GREEN);
    txt(0, 4, 11, "CAMPAIGN", sel == 0 ? PAL_GREEN : PAL_UI);
    if (save.sector > 1) {
        put_tile(0, 13, 11, sel == 0 ? FONT_TILE('<') : TILE_BLANK, PAL_DIM);
        txt(0, 14, 11, "S", PAL_DIM);
        txt_num(0, 15, 11, sel_sector, 1, PAL_PURPLE);
        put_tile(0, 16, 11, sel == 0 ? FONT_TILE('>') : TILE_BLANK, PAL_DIM);
    }
    if (endless_open()) {
        txt(0, 4, 12, "ENDLESS", sel == 1 ? PAL_GREEN : PAL_UI);
        txt(0, 4, 13, "ENTER SEED", sel == 2 ? PAL_GREEN : PAL_UI);
    } else {
        txt(0, 4, 12, "ENDLESS", PAL_DIM);
        txt(0, 12, 12, "LOCKED", PAL_DIM);
        txt(0, 4, 13, "CLEAR SECTOR 4", PAL_DIM);
    }
}

/* returns 0 CAMPAIGN (sel_sector), 1 ENDLESS, 2 SEED, or 3 a test start */
static uint8_t title(void)
{
    uint8_t sel = 0, t = 0, started = 0, n = endless_open() ? 3 : 1;
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
    sel_sector = save.sector ? save.sector : 1;
    if (sel_sector > sector_count()) sel_sector = sector_count();
    for (;;) {
        wait_vbl_done();
        input_poll();
        fx_update();
        title_fx(t++);
        if (dbg_mode) return 3;
        if (!started) {
            txt(0, 5, 12, (t & 32) ? "PRESS START" : "           ", PAL_GREEN);
            if (joy_new & (J_START | J_A)) {
                started = 1;
                sfx_play(SFX_SELECT);
                gfill(0, 0, 12, 20, 1, TILE_BLANK, 0);
                menu_draw(sel);
            }
            continue;
        }
        if (joy_new & (J_UP | J_DOWN | J_LEFT | J_RIGHT)) {
            if (joy_new & J_UP) sel = (uint8_t)(sel ? sel - 1 : n - 1);
            if (joy_new & J_DOWN) sel = (uint8_t)(sel + 1 < n ? sel + 1 : 0);
            if (sel == 0 && (joy_new & J_LEFT) && sel_sector > 1) sel_sector--;
            if (sel == 0 && (joy_new & J_RIGHT) && sel_sector < save.sector && sel_sector < sector_count()) sel_sector++;
            sfx_play(SFX_MENU);
            menu_draw(sel);
        }
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

/* returns 1 to continue (the same sector or stage, three new mechs) */
static uint8_t game_over(void)
{
    uint8_t t, cont = 0;
    gfx_hide_sprites();
    part_clear();
    fade_to(4);
    HIDE_WIN;
    scroll_x = scroll_y = 0;
    cam_x = 0;
    cam_y = 0;
    clear_bkg();
    music_play(SONG_GAMEOVER);
    txt(0, 5, 2, "GAME OVER", PAL_HAZARD);
    txt(0, 3, 5, run.mode == MODE_CAMPAIGN ? "SECTOR" : "STAGE", PAL_DIM);
    txt_num(0, 13, 5, run.stage, 4, PAL_UI);
    txt(0, 3, 7, "SCORE", PAL_DIM);
    {
        /* up to 9 999 999 */
        uint32_t sc = run.score;
        txt_num(0, 10, 7, (uint16_t)(sc / 1000u), 4, PAL_GREEN);
        txt_num(0, 14, 7, (uint16_t)(sc % 1000u), 3, PAL_GREEN);
    }
    txt(0, 3, 9, "CELLS", PAL_DIM);
    txt_num(0, 13, 9, run.cells, 4, PAL_UI);
    if (run.mode == MODE_ENDLESS) {
        txt(0, 3, 11, "SEED", PAL_DIM);
        txt_hex(0, 13, 11, run.seed, PAL_PURPLE);
    }
    if (run.score > save.best_score) {
        txt(0, 4, 13, "NEW RECORD!", PAL_GREEN);
        save.best_score = run.score;
    }
    if (run.mode == MODE_ENDLESS && run.stage > save.best_stage) save.best_stage = run.stage;
    save_write();
    txt(0, 3, 15, "A CONTINUE", PAL_UI);
    txt(0, 3, 16, "B TITLE", PAL_DIM);
    fade_to(0);
    for (t = 0; ; t++) {
        wait_vbl_done();
        input_poll();
        fx_update();
        if ((t & 31) == 0) part_burst((uint16_t)(20 + (fx_rand() & 127)), (uint8_t)(20 + (fx_rand() & 63)), 6, SPR_STAR, SPAL_SPARK_P, 12, 0);
        if (t > 60 && (joy_new & (J_A | J_START))) { cont = 1; sfx_play(SFX_SELECT); break; }
        if (t > 60 && (joy_new & J_B)) break;
    }
    fade_to(4);
    return cont;
}

/* the campaign is over: the station is aligned */
static void ending(void)
{
    static const char *const lines[] = {
        "THE CORE IS SILENT.",
        "",
        "GRAVITY HOLDS ITS",
        "COURSE AGAIN, AND",
        "THE STATION TURNS",
        "TOWARD THE SUN.",
        "",
        "ALIGNMENT RESTORED",
        "",
        "THANK YOU, PILOT.",
        0
    };
    uint8_t i, t = 0;
    gfx_hide_sprites();
    part_clear();
    fade_to(4);
    HIDE_WIN;
    scroll_x = scroll_y = 0;
    cam_x = 0;
    cam_y = 0;
    clear_bkg();
    music_play(SONG_ENDING);
    for (i = 0; lines[i]; i++)
        txt(0, (uint8_t)((20 - strlen(lines[i])) >> 1), (uint8_t)(2 + i), lines[i], i == 7 ? PAL_GREEN : PAL_UI);
    txt(0, 3, 14, "SCORE", PAL_DIM);
    txt_num(0, 10, 14, (uint16_t)(run.score / 100u > 65535u ? 65535u : run.score / 100u), 5, PAL_GREEN);
    txt(0, 15, 14, "00", PAL_GREEN);
    txt(0, 1, 16, "ENDLESS IS OPEN", PAL_PURPLE);
    if (run.score > save.best_score) save.best_score = run.score;
    save.campaign_done = 1;
    save_write();
    fade_to(0);
    for (;; t++) {
        wait_vbl_done();
        input_poll();
        fx_update();
        title_fx(t);
        if ((t & 15) == 0) part_burst((uint16_t)(8 + (fx_rand() & 127)), (uint8_t)(8 + (fx_rand() & 127)), 6, SPR_STAR, (t & 16) ? SPAL_SPARK_G : SPAL_SPARK_P, 12, 0);
        if (t > 240 && (joy_new & (J_A | J_START))) break;
    }
    fade_to(4);
}

void game_main(void) BANKED
{
    uint8_t m, r;
    uint16_t seed = 0;
    save_load();
    for (;;) {
        cam_x = 0;
        cam_y = 0;
        m = title();
        if (m == 3) {
            /* (a ROM test) */
            m = dbg_mode == 1 ? 0 : 1;
            seed = dbg_seed;
            sel_sector = (uint8_t)dbg_stage;
            dbg_mode = 0;
        } else if (m == 2) {
            seed = seed_from_time();
            if (!seed_entry(&seed)) continue;
        } else if (m == 1) {
            seed = seed_from_time();
            dbg_stage = 1;
        }
        fade_to(4);
        gfx_hide_sprites();
        part_clear();
        clear_bkg();
        if (m == 0) run_start(&run, MODE_CAMPAIGN, 0, sel_sector);
        else run_start(&run, MODE_ENDLESS, seed, dbg_stage ? dbg_stage : 1);
        dbg_stage = 0;
        for (;;) {
            r = play_run();
            if (r == PLAY_WIN) {
                ending();
                break;
            }
            if (r != PLAY_OVER || !game_over()) break;
            /* continue: the same sector or stage, three new mechs, the score halved */
            run.lives = RUN_LIVES;
            run.score >>= 1;
            fade_to(4);
            clear_bkg();
        }
        fade_to(4);
    }
}
