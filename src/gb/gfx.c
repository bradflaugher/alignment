/* ALIGNMENT - drawing helpers, palettes, effects, particles and the VBlank handler.
 * Bank 0. Every art table lives in a switchable bank (assets.c); the small lookup
 * tables the hot paths need are copied to WRAM once at boot. */
#include <gb/gb.h>
#include <gb/cgb.h>
#include <string.h>
#include "gfx.h"
#include "assets.h"
#include "sound.h"
#include "render.h"

BANKREF_EXTERN(assets)

#define ASSETS_IN()  uint8_t _bank = CURRENT_BANK; SWITCH_ROM(BANK(assets))
#define ASSETS_OUT() SWITCH_ROM(_bank)

uint8_t is_cgb;
volatile uint8_t frame;
uint8_t joy, joy_prev, joy_new;
uint8_t scroll_x, scroll_y;
int8_t shake_x, shake_y;

/* RAM copies of banked tables (render.c reads them) */
uint8_t r_wall_tile[4 * WALL_VARIANTS];
uint8_t r_mt_tiles[NUM_MT * 4];
uint8_t r_mt_pal[NUM_MT];

static uint16_t bg_pal[8 * 4];
static uint16_t spr_pal[8 * 4];
static uint16_t out_pal[8 * 4];
static uint8_t fade_level;
static uint8_t shake_amt;
static uint8_t flash_t, flash_white;
static volatile uint8_t pal_dirty;
static uint8_t rnd_s = 0x5A;

/* ------------------------------------------------------------ VBlank */

static void vbl_isr(void)
{
    frame++;
    SCX_REG = (uint8_t)(scroll_x + shake_x);
    SCY_REG = (uint8_t)(scroll_y + shake_y);
    if (pal_dirty && is_cgb) {
        set_bkg_palette(0, 8, out_pal);
        pal_dirty = 0;
    }
    sound_tick();
}

uint8_t fx_rand(void)
{
    rnd_s ^= (uint8_t)(rnd_s << 3);
    rnd_s ^= (uint8_t)(rnd_s >> 5);
    rnd_s ^= (uint8_t)(rnd_s << 1);
    return (uint8_t)(rnd_s + frame);
}

void input_poll(void)
{
    joy_prev = joy;
    joy = joypad();
    joy_new = (uint8_t)(joy & ~joy_prev);
}

void wait_frames(uint8_t n)
{
    while (n--) {
        wait_vbl_done();
        fx_update();
    }
}

/* ------------------------------------------------------------ tiles */

void bg_tile_data(uint8_t first, uint8_t n, const uint8_t *src)
{
    uint8_t k;
    /* split at 128: low indices live at 0x9000, high ones at 0x8800 */
    if (first < 128) {
        k = (uint8_t)(128 - first);
        if (k > n) k = n;
        set_data((uint8_t *)(0x9000u + ((uint16_t)first << 4)), src, (uint16_t)k << 4);
        src += (uint16_t)k << 4;
        n -= k;
        first = 128;
    }
    if (n)
        set_data((uint8_t *)(0x8800u + ((uint16_t)(first - 128) << 4)), src, (uint16_t)n << 4);
}

static void load_common(void)
{
    ASSETS_IN();
    bg_tile_data(TILE_FONT_BASE, 64, font_tiles);
    bg_tile_data(TILE_UI_BASE, 16, ui_tiles);
    set_sprite_data(0, NUM_SPR_TILES, spr_tiles);
    memcpy(r_wall_tile, wall_tile, sizeof(r_wall_tile));
    memcpy(r_mt_tiles, mt_tiles, sizeof(r_mt_tiles));
    memcpy(r_mt_pal, mt_cgb_pal, sizeof(r_mt_pal));
    memcpy(bg_pal, bg_cgb_pal, sizeof(bg_pal));
    memcpy(spr_pal, spr_cgb_pal, sizeof(spr_pal));
    ASSETS_OUT();
}

void gfx_load_play_tiles(void)
{
    ASSETS_IN();
    bg_tile_data(TILE_ART_BASE, NUM_ART_TILES, art_tiles);
    ASSETS_OUT();
    anim_reset();
}

void gfx_load_title_tiles(void)
{
    ASSETS_IN();
    bg_tile_data(TILE_ART_BASE, LOGO_NTILES, logo_tiles);
    ASSETS_OUT();
}

void gfx_hide_sprites(void)
{
    uint8_t i;
    for (i = 0; i < 40; i++) move_sprite(i, 0, 0);
}

void gfx_init(void)
{
    is_cgb = (uint8_t)(_cpu == CGB_TYPE);
    if (is_cgb) cpu_fast();
    DISPLAY_OFF;
    /* BG + window tiles from 0x8800 (signed), BG map 9800, window map 9C00, 8x8 sprites */
    LCDC_REG = LCDCF_OFF | LCDCF_WIN9C00 | LCDCF_WINOFF | LCDCF_BG8800 | LCDCF_BG9800 |
               LCDCF_OBJ8 | LCDCF_OBJON | LCDCF_BGON;
    BGP_REG = DMG_BGP;
    OBP0_REG = DMG_OBP0;
    OBP1_REG = DMG_OBP1;
    load_common();
    clear_bkg();
    clear_win();
    gfx_hide_sprites();
    fade_level = 0;
    pal_apply();
    if (is_cgb) set_sprite_palette(0, 8, spr_pal);
    scroll_x = scroll_y = 0;
    CRITICAL {
        add_VBL(vbl_isr);
    }
    set_interrupts(VBL_IFLAG);
    DISPLAY_ON;
}

/* ------------------------------------------------------------ text */

static void put_tiles(uint8_t win, uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint8_t *t)
{
    if (win) set_win_tiles(x, y, w, h, t);
    else set_bkg_tiles(x, y, w, h, t);
}

static void put_attr(uint8_t win, uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint8_t *a)
{
    if (!is_cgb) return;
    VBK_REG = VBK_ATTRIBUTES;
    put_tiles(win, x, y, w, h, a);
    VBK_REG = VBK_TILES;
}

void put_tile(uint8_t win, uint8_t x, uint8_t y, uint8_t tile, uint8_t pal)
{
    put_attr(win, x, y, 1, 1, &pal);
    put_tiles(win, x, y, 1, 1, &tile);
}

void gfill(uint8_t win, uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t tile, uint8_t pal)
{
    uint8_t row[32], i, j;
    if (w > 32) w = 32;
    memset(row, pal, w);
    for (j = 0; j < h; j++) put_attr(win, x, (uint8_t)(y + j), w, 1, row);
    memset(row, tile, w);
    for (i = 0; i < h; i++) put_tiles(win, x, (uint8_t)(y + i), w, 1, row);
}

void txt(uint8_t win, uint8_t x, uint8_t y, const char *s, uint8_t pal)
{
    uint8_t buf[32], n = 0;
    while (*s && n < 32) {
        char c = *s++;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c < 32 || c > 95) c = ' ';
        buf[n++] = FONT_TILE(c);
    }
    if (!n) return;
    if (is_cgb) {
        uint8_t a[32];
        memset(a, pal, n);
        put_attr(win, x, y, n, 1, a);
    }
    put_tiles(win, x, y, n, 1, buf);
}

void txt_num(uint8_t win, uint8_t x, uint8_t y, uint16_t v, uint8_t digits, uint8_t pal)
{
    char b[6];
    uint8_t i = digits;
    if (i > 5) i = 5;
    b[i] = 0;
    while (i--) {
        b[i] = (char)('0' + v % 10);
        v /= 10;
    }
    txt(win, x, y, b, pal);
}

void txt_hex(uint8_t win, uint8_t x, uint8_t y, uint16_t v, uint8_t pal)
{
    char b[5];
    uint8_t i, d;
    for (i = 0; i < 4; i++) {
        d = (uint8_t)((v >> (12 - 4 * i)) & 15);
        b[i] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
    }
    b[4] = 0;
    txt(win, x, y, b, pal);
}

void draw_box(uint8_t win, uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t pal)
{
    uint8_t i;
    gfill(win, x, y, w, h, TILE_BLANK, pal);
    put_tile(win, x, y, UI_BOX_TL, pal);
    put_tile(win, (uint8_t)(x + w - 1), y, UI_BOX_TR, pal);
    put_tile(win, x, (uint8_t)(y + h - 1), UI_BOX_BL, pal);
    put_tile(win, (uint8_t)(x + w - 1), (uint8_t)(y + h - 1), UI_BOX_BR, pal);
    for (i = 1; i < w - 1; i++) {
        put_tile(win, (uint8_t)(x + i), y, UI_BOX_T, pal);
        put_tile(win, (uint8_t)(x + i), (uint8_t)(y + h - 1), UI_BOX_B, pal);
    }
    for (i = 1; i < h - 1; i++) {
        put_tile(win, x, (uint8_t)(y + i), UI_BOX_L, pal);
        put_tile(win, (uint8_t)(x + w - 1), (uint8_t)(y + i), UI_BOX_R, pal);
    }
}

void clear_bkg(void)
{
    gfill(0, 0, 0, 32, 32, TILE_BLANK, 0);
}

void clear_win(void)
{
    gfill(1, 0, 0, 20, 18, TILE_BLANK, 0);
}

void put_block(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint8_t *tiles, const uint8_t *attrs)
{
    if (is_cgb) {
        VBK_REG = VBK_ATTRIBUTES;
        set_bkg_tiles(x, y, w, h, attrs);
        VBK_REG = VBK_TILES;
    }
    set_bkg_tiles(x, y, w, h, tiles);
}

/* ------------------------------------------------------------ palettes */

static uint16_t scale(uint16_t c, uint8_t l)
{
    uint8_t r = (uint8_t)(c & 31), g = (uint8_t)((c >> 5) & 31), b = (uint8_t)((c >> 10) & 31);
    uint8_t k = (uint8_t)(4 - l);
    r = (uint8_t)((r * k) >> 2);
    g = (uint8_t)((g * k) >> 2);
    b = (uint8_t)((b * k) >> 2);
    return (uint16_t)(r | ((uint16_t)g << 5) | ((uint16_t)b << 10));
}

static uint8_t dmg_shift(uint8_t pal, uint8_t l)
{
    /* each 2-bit shade moves l steps towards black (3) */
    uint8_t out = 0, i, s;
    for (i = 0; i < 4; i++) {
        s = (uint8_t)((pal >> (i * 2)) & 3);
        s = (uint8_t)(s + l > 3 ? 3 : s + l);
        out |= (uint8_t)(s << (i * 2));
    }
    return out;
}

void pal_apply(void)
{
    uint8_t i;
    if (!is_cgb) {
        BGP_REG = dmg_shift(DMG_BGP, fade_level);
        OBP0_REG = dmg_shift(DMG_OBP0, fade_level);
        OBP1_REG = dmg_shift(DMG_OBP1, fade_level);
        return;
    }
    for (i = 0; i < 32; i++) {
        uint16_t c = bg_pal[i];
        if (flash_t && (i & 3)) c = flash_white ? 0x7FFF : c | 0x1084;
        out_pal[i] = fade_level ? scale(c, fade_level) : c;
    }
    pal_dirty = 1;
    {
        uint16_t sp[32];
        for (i = 0; i < 32; i++) sp[i] = fade_level ? scale(spr_pal[i], fade_level) : spr_pal[i];
        wait_vbl_done();
        set_sprite_palette(0, 8, sp);
    }
}

void pal_set_deck(uint8_t deck)
{
    ASSETS_IN();
    memcpy(&bg_pal[PAL_WALL * 4], &deck_cgb_pal[(deck % NUM_DECK_PALS) * 4], 8);
    ASSETS_OUT();
    pal_apply();
}

void pal_gravity(uint8_t g)
{
    (void)g;    /* the mech picks SPAL_MECH_P / SPAL_MECH_G per sprite */
}

void fade_to(uint8_t level)
{
    while (fade_level != level) {
        if (fade_level < level) fade_level++;
        else fade_level--;
        pal_apply();
        wait_frames(3);
    }
}

void flash(uint8_t frames, uint8_t white)
{
    flash_t = frames;
    flash_white = white;
    if (is_cgb) pal_apply();
    else BGP_REG = white ? 0x00 : 0x1A;
}

/* ------------------------------------------------------------ effects */

void fx_shake(uint8_t amount)
{
    if (amount > shake_amt) shake_amt = amount;
}

void fx_update(void)
{
    if (shake_amt) {
        shake_x = (int8_t)((fx_rand() % (shake_amt * 2 + 1)) - shake_amt);
        shake_y = (int8_t)((fx_rand() % (shake_amt * 2 + 1)) - shake_amt);
        if (!(frame & 1)) shake_amt--;
    } else {
        shake_x = shake_y = 0;
    }
    if (flash_t) {
        if (!--flash_t) {
            if (is_cgb) pal_apply();
            else BGP_REG = dmg_shift(DMG_BGP, fade_level);
        }
    }
    part_update();
    anim_update();
}

/* ------------------------------------------------------------ particles */

static uint8_t p_life[NUM_PART], p_tile[NUM_PART], p_pal[NUM_PART];
static int16_t p_x[NUM_PART], p_y[NUM_PART];    /* 8.4 fixed point screen coords */
static int8_t p_vx[NUM_PART], p_vy[NUM_PART], p_g[NUM_PART];
static uint8_t p_next;

void part_clear(void)
{
    uint8_t i;
    for (i = 0; i < NUM_PART; i++) {
        p_life[i] = 0;
        move_sprite((uint8_t)(SP_PART + i), 0, 0);
    }
}

void part_spawn(uint8_t px, uint8_t py, int8_t vx, int8_t vy, uint8_t tile, uint8_t pal, uint8_t life)
{
    uint8_t i = p_next;
    p_next = (uint8_t)((p_next + 1) % NUM_PART);
    p_x[i] = (int16_t)px << 4;
    p_y[i] = (int16_t)py << 4;
    p_vx[i] = vx;
    p_vy[i] = vy;
    p_g[i] = 0;
    p_tile[i] = tile;
    p_pal[i] = pal;
    p_life[i] = life;
    set_sprite_tile((uint8_t)(SP_PART + i), tile);
    set_sprite_prop((uint8_t)(SP_PART + i), pal);
}

void part_burst(uint8_t px, uint8_t py, uint8_t n, uint8_t tile, uint8_t pal, uint8_t speed, int8_t gravity)
{
    uint8_t k;
    while (n--) {
        k = p_next;
        part_spawn(px, py, (int8_t)((fx_rand() % (speed * 2 + 1)) - speed),
                   (int8_t)((fx_rand() % (speed * 2 + 1)) - speed), tile, pal,
                   (uint8_t)(14 + (fx_rand() & 15)));
        p_g[k] = gravity;
    }
}

void part_update(void)
{
    uint8_t i, s;
    for (i = 0; i < NUM_PART; i++) {
        s = (uint8_t)(SP_PART + i);
        if (!p_life[i]) continue;
        if (!--p_life[i]) {
            move_sprite(s, 0, 0);
            continue;
        }
        p_x[i] += p_vx[i];
        p_y[i] += p_vy[i];
        if (p_g[i] && (frame & 1)) p_vy[i] = (int8_t)(p_vy[i] + p_g[i]);
        if (p_life[i] < 6 && (frame & 1)) {
            move_sprite(s, 0, 0);       /* flicker out */
        } else {
            move_sprite(s, (uint8_t)((p_x[i] >> 4) + 8 - shake_x), (uint8_t)((p_y[i] >> 4) + 16 - shake_y));
        }
    }
}

/* ------------------------------------------------------------ tile animations */

static uint8_t a_frame[NUM_ANIM], a_timer[NUM_ANIM];
static uint8_t a_frames_r[NUM_ANIM], a_speed_r[NUM_ANIM], a_first[NUM_ANIM];
static uint8_t a_cursor;

void anim_reset(void)
{
    uint8_t i;
    ASSETS_IN();
    for (i = 0; i < NUM_ANIM; i++) {
        a_frame[i] = 0;
        a_timer[i] = (uint8_t)(i * 3);
        a_frames_r[i] = anim_frames[i];
        a_speed_r[i] = anim_speed[i];
        a_first[i] = mt_tiles[anim_mt[i] * 4];
    }
    ASSETS_OUT();
}

void anim_update(void)
{
    /* every timer runs; at most one animation is copied per frame (64 bytes) */
    uint8_t i, n, done = 0;
    for (i = 0; i < NUM_ANIM; i++)
        if (a_timer[i] < 255) a_timer[i]++;
    for (n = 0; n < NUM_ANIM && !done; n++) {
        i = a_cursor;
        a_cursor = (uint8_t)((a_cursor + 1) % NUM_ANIM);
        if (a_timer[i] < a_speed_r[i]) continue;
        a_timer[i] = 0;
        a_frame[i] = (uint8_t)((a_frame[i] + 1) % a_frames_r[i]);
        {
            ASSETS_IN();
            bg_tile_data(a_first[i], 4, anim_data[i] + ((uint16_t)a_frame[i] << 6));
            ASSETS_OUT();
        }
        done = 1;
    }
}
