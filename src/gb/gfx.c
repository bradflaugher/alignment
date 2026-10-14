/* ALIGNMENT - drawing helpers, palettes, effects, particles and the VBlank handler.
 * Bank 0. Every art table lives in a switchable bank (assets.c); the small lookup
 * tables the hot paths need are copied to WRAM once at boot. */
#include <gb/gb.h>
#include <gb/cgb.h>
#include <string.h>
#include "gfx.h"
#include "assets.h"
#include "sound.h"

BANKREF_EXTERN(assets)

#define ASSETS_IN()  uint8_t _bank = CURRENT_BANK; SWITCH_ROM(BANK(assets))
#define ASSETS_OUT() SWITCH_ROM(_bank)

uint8_t is_cgb;
volatile uint8_t frame;
uint8_t joy, joy_prev, joy_new;
uint8_t scroll_x, scroll_y;
#define SCY_MAX (256 - HUD_WY)    /* the lowest camera: the HUD covers the rest */
int8_t shake_x, shake_y;

/* RAM copies of banked tables (render.c reads them) */
uint8_t r_wall_tile[4 * WALL_VARIANTS];
uint8_t r_mt_tiles[NUM_MT * 4];
uint8_t r_mt_pal[NUM_MT];

static uint16_t bg_pal[8 * 4];
static uint16_t spr_pal[8 * 4];
uint8_t hud_t[20], hud_a[20];
static volatile uint8_t row_dirty;
static uint16_t out_pal[8 * 4];
static uint16_t out_spr[8 * 4];
static uint8_t spr_level, spr_force = 1;
static uint8_t fade_level;
static uint8_t shake_amt;
static uint8_t flash_t, flash_white;
static volatile uint8_t pal_dirty;
static uint8_t rnd_s = 0x5A;
static uint8_t a_on;            /* tile animations run (room art loaded) */
static uint8_t a_door_off;      /* the laser gates are off: their animation rests */

/* ------------------------------------------------------------ streaming */

/* The whole stage's tile map is built once (render_prebuild) into cartridge RAM: bank 2
 * holds the tiles, bank 3 the CGB attributes, 64 bytes per stage column (two 32-tile map
 * columns). Streaming a column is then a copy from there in VBlank. The game sets
 * stream_on only while it is not using cartridge RAM itself. */
#define COLQ 8
static uint8_t cq_c[COLQ];
static volatile uint8_t cq_head, cq_n;
volatile uint8_t stream_on;

uint8_t col_queued(void)
{
    return cq_n;
}

void col_push(uint8_t cx)
{
    while (cq_n >= COLQ) wait_vbl_done();
    cq_c[(uint8_t)((cq_head + cq_n) & (COLQ - 1))] = cx;
    cq_n++;
}

/* 32 bytes down a map column (stride 32): de = dst, bc = src (sdcccall 1) */
static void copy_col(uint8_t *dst, const uint8_t *src) __naked
{
    (void)dst;
    (void)src;
    __asm
        ld  h, d
        ld  l, e
        ld  e, #32
    00001$:
        ld  a, (bc)
        inc bc
        ld  (hl), a
        ld  a, l
        add a, #32
        ld  l, a
        jr  nc, 00002$
        inc h
    00002$:
        dec e
        jr  nz, 00001$
        ret
    __endasm;
}

static void col_write(void)
{
    uint8_t cx = cq_c[cq_head];
    uint8_t *d = (uint8_t *)(0x9800u + ((cx << 1) & 31));
    const uint8_t *src = (const uint8_t *)(0xA000u + ((uint16_t)cx << 6));
    SWITCH_RAM(2);
    copy_col(d, src);
    copy_col(d + 1, src + 32);
    if (is_cgb) {
        SWITCH_RAM(3);
        VBK_REG = 1;
        copy_col(d, src);
        copy_col(d + 1, src + 32);
        VBK_REG = 0;
    }
    cq_head = (uint8_t)((cq_head + 1) & (COLQ - 1));
    cq_n--;
}

/* 4-tile art copies (animations, phase / laser swaps), also done in VBlank: the
 * source is in the art bank */
#define TQ 4
static const uint8_t *tq_src[TQ];
static uint8_t *tq_dst[TQ];
static volatile uint8_t tq_head, tq_n;

static void tq_push(uint8_t tile, const uint8_t *src)
{
    uint8_t i;
    while (tq_n >= TQ) wait_vbl_done();
    i = (uint8_t)((tq_head + tq_n) & (TQ - 1));
    tq_src[i] = src;
    tq_dst[i] = (uint8_t *)(tile < 128 ? 0x9000u + ((uint16_t)tile << 4) : 0x8000u + ((uint16_t)tile << 4));
    tq_n++;
}

static void tq_write(void)
{
    uint8_t b = CURRENT_BANK;
    SWITCH_ROM(BANK(assets));
    {
        /* 4 tiles; BG tiles 127 and 128 aren't next to each other in VRAM (0x97F0 /
         * 0x8800), so a run of 4 across them is copied in two parts */
        uint8_t *d = tq_dst[tq_head];
        const uint8_t *src = tq_src[tq_head];
        if (d >= (uint8_t *)0x97D0u && d < (uint8_t *)0x9800u) {
            uint8_t n = (uint8_t)(0x9800u - (uint16_t)d);
            memcpy(d, src, n);
            memcpy((uint8_t *)0x8800u, src + n, (uint8_t)(64 - n));
        } else {
            memcpy(d, src, 64);
        }
    }
    SWITCH_ROM(b);
    tq_head = (uint8_t)((tq_head + 1) & (TQ - 1));
    tq_n--;
}

/* ------------------------------------------------------------ VBlank */

static void vbl_isr(void)
{
    /* a DMG's VBlank fits one column or one art copy or the HUD row; a Color's twice that */
    if (row_dirty) {
        uint8_t *d = (uint8_t *)0x9C00u, i;
        for (i = 0; i < 20; i++) d[i] = hud_t[i];
        if (is_cgb) {
            VBK_REG = 1;
            for (i = 0; i < 20; i++) d[i] = hud_a[i];
            VBK_REG = 0;
        }
        row_dirty = 0;
    } else if (cq_n && stream_on) {
        col_write();
        if (is_cgb) {
            if (cq_n) col_write();
            else if (tq_n) tq_write();
        }
    } else if (tq_n) {
        tq_write();
        if (is_cgb && tq_n) tq_write();
    }
    frame++;
    SCX_REG = (uint8_t)(scroll_x + shake_x);
    {
        /* a shake never shows past the stage's top or bottom (the map would wrap) */
        uint8_t y = (uint8_t)(scroll_y + shake_y);
        if (shake_y < 0 && y > scroll_y) y = 0;
        else if (shake_y > 0 && y > SCY_MAX && scroll_y <= SCY_MAX) y = SCY_MAX;
        SCY_REG = y;
    }
    if (pal_dirty && is_cgb) {
        /* (both palettes are written here, in VBlank, so nobody waits for one) */
        set_bkg_palette(0, 8, out_pal);
        if (pal_dirty & 2) set_sprite_palette(0, 8, out_spr);
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

static void set_vram_tile(uint8_t i, const uint8_t *src);

/* Big loads happen with the LCD off (title / room art); small ones (animation frames,
 * always tiles >= 128) go through set_sprite_data: tiles 128-255 are shared by sprites
 * and the BG, and that routine waits for VRAM to be free. */
void bg_tile_data(uint8_t first, uint8_t n, const uint8_t *src)
{
    uint8_t k;
    if (first >= 128) {
        set_sprite_data(first, n, src);
        return;
    }
    k = (uint8_t)(128 - first);
    if (k > n) k = n;
    if (LCDC_REG & LCDCF_ON) {
        /* (never happens with the LCD on in practice; keep it safe anyway) */
        uint8_t i;
        for (i = 0; i < k; i++)
            set_vram_tile(first + i, src + ((uint16_t)i << 4));
    } else {
        memcpy((uint8_t *)(0x9000u + ((uint16_t)first << 4)), src, (uint16_t)k << 4);
    }
    if (n > k) set_sprite_data(128, (uint8_t)(n - k), src + ((uint16_t)k << 4));
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
    spr_force = 1;
    ASSETS_OUT();
}

static void set_vram_tile(uint8_t i, const uint8_t *src)
{
    uint8_t *d = (uint8_t *)(0x9000u + ((uint16_t)i << 4));
    uint8_t j;
    for (j = 0; j < 16; j++) set_vram_byte(d + j, src[j]);
}

/* both called with the screen faded to black: the LCD goes off for the copy */
void gfx_load_play_tiles(void)
{
    ASSETS_IN();
    DISPLAY_OFF;
    bg_tile_data(TILE_ART_BASE, NUM_ART_TILES, art_tiles);
    DISPLAY_ON;
    ASSETS_OUT();
    anim_reset();
}

void gfx_load_title_tiles(void)
{
    ASSETS_IN();
    a_on = 0;
    DISPLAY_OFF;
    bg_tile_data(TILE_ART_BASE, LOGO_NTILES, logo_tiles);
    DISPLAY_ON;
    ASSETS_OUT();
}

/* bank 0: it maps the art bank in, which banked code must never do */
void gfx_draw_logo(uint8_t y0)
{
    uint8_t y;
    ASSETS_IN();
    for (y = 0; y < LOGO_H; y++)
        put_block(0, (uint8_t)(y0 + y), LOGO_W, 1, logo_map + y * LOGO_W, logo_cgb_attr + y * LOGO_W);
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

void hud_commit(void)
{
    row_dirty = 1;
}

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
    /* digits by subtraction (a 16-bit division costs a DMG ten scanlines) */
    static const uint16_t p10[6] = { 1, 10, 100, 1000, 10000, 0 };
    char b[6];
    uint8_t i = digits, d;
    uint16_t p;
    if (i > 5) i = 5;
    for (d = 4; d >= i && d < 5; d--) while (v >= p10[d]) v -= p10[d];   /* keep the low digits */
    b[i] = 0;
    for (d = 0; d < i; d++) {
        char c = '0';
        p = p10[i - 1 - d];
        while (v >= p) { v -= p; c++; }
        b[d] = c;
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
    /* sprites only change with the fade (a flash leaves them be) */
    if (spr_level != fade_level || spr_force) {
        spr_level = fade_level;
        spr_force = 0;
        for (i = 0; i < 32; i++) out_spr[i] = fade_level ? scale(spr_pal[i], fade_level) : spr_pal[i];
        pal_dirty = 3;
    } else {
        pal_dirty |= 1;
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
        /* (masks, not %: a division costs a DMG a tenth of a frame) */
        shake_x = (int8_t)((int8_t)(fx_rand() & 3) - 1);
        shake_y = (int8_t)((int8_t)(fx_rand() & 3) - 1);
        if (shake_amt > 2) { shake_x = (int8_t)(shake_x * 2); shake_y = (int8_t)(shake_y * 2); }
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

/* Particles live in world pixels and are drawn relative to the camera. Speeds are in
 * 1/4 px a frame, kept in a 2-bit fraction; all the per-frame maths is 8-bit and the
 * sprites are written straight into the shadow OAM. A DMG moves half of them each frame
 * (twice as far): it can't afford all of them. */
uint16_t cam_x;
uint8_t cam_y;
/* particles: positions in 1/4 px, velocities in 1/4 px per frame; the update is
 * hand-written (the C version cost a DMG a fifth of a frame) and relies on this layout */
typedef struct {
    uint8_t life;
    int8_t vx;
    uint16_t x;
    int8_t g;
    int8_t vy;
    uint16_t y;
} Part;
static Part part[NUM_PART];
static uint8_t p_next;
static uint16_t pk_x;       /* screen x = (x >> 2) - pk_x */
static uint8_t pk_y, pk_blink, p_count;
static OAM_item_t *p_oam;

void part_clear(void)
{
    uint8_t i;
    for (i = 0; i < NUM_PART; i++) {
        part[i].life = 0;
        shadow_OAM[SP_PART + i].y = 0;
    }
}

void part_spawn(uint16_t px, uint8_t py, int8_t vx, int8_t vy, uint8_t tile, uint8_t pal, uint8_t life)
{
    Part *p = &part[p_next];
    OAM_item_t *o = &shadow_OAM[SP_PART + p_next];
    if (++p_next == NUM_PART) p_next = 0;
    p->x = px << 2;
    p->y = (uint16_t)py << 2;
    p->vx = vx;
    p->vy = vy;
    p->g = 0;
    p->life = life;
    o->tile = tile;
    o->prop = pal;
    o->y = 0;
}

void part_burst(uint16_t px, uint8_t py, uint8_t n, uint8_t tile, uint8_t pal, uint8_t speed, int8_t gravity)
{
    /* written out (no part_spawn calls, 8-bit maths only): a burst lands on a busy frame */
    uint8_t mask = 1, half, i = p_next;
    uint16_t x4 = px << 2, y4 = (uint16_t)py << 2;
    Part *p = &part[i];
    OAM_item_t *o = &shadow_OAM[SP_PART + i];
    if (!is_cgb) n = (uint8_t)((n + 1) >> 1);
    while (mask < speed) mask = (uint8_t)((mask << 1) | 1);    /* speed..2*speed, a power of two - 1 */
    half = (uint8_t)(mask >> 1);
    while (n--) {
        uint8_t r = fx_rand();
        p->x = x4;
        p->y = y4;
        p->vx = (int8_t)(uint8_t)((r & mask) - half);
        p->vy = (int8_t)(uint8_t)(((uint8_t)((r >> 4) | (r << 4)) & mask) - half);
        p->g = gravity;
        p->life = (uint8_t)(14 + (r & 15));
        o->tile = tile;
        o->prop = pal;
        o->y = 0;
        p++;
        o++;
        if (++i == NUM_PART) {
            i = 0;
            p = part;
            o = &shadow_OAM[SP_PART];
        }
    }
    p_next = i;
}

static void part_asm(void) __naked
{
    __asm
        ld  hl, #_part
        ld  a, #NUM_PART
        ld  (_p_count), a
        ld  a, #<(_shadow_OAM + SP_PART * 4)
        ld  (_p_oam), a
        ld  a, #>(_shadow_OAM + SP_PART * 4)
        ld  (_p_oam + 1), a
    00001$:
        push hl
        ld  a, (hl)
        or  a
        jp  z, 00008$
        dec a
        ld  (hl+), a
        jp  z, 00007$
        ; x += vx
        ld  a, (hl+)
        ld  c, a
        rlca
        sbc a, a
        ld  b, a
        ld  a, c
        add a, (hl)
        ld  (hl+), a
        ld  a, b
        adc a, (hl)
        ld  (hl+), a
        ; vy += g
        ld  a, (hl+)
        add a, (hl)
        ld  (hl+), a
        ; y += vy
        ld  c, a
        rlca
        sbc a, a
        ld  b, a
        ld  a, c
        add a, (hl)
        ld  (hl+), a
        ld  c, a
        ld  a, b
        adc a, (hl)
        ld  b, a
        ld  (hl), a
        ; screen y
        ld  a, c
        srl b
        rra
        srl b
        rra
        inc b
        dec b
        jp  nz, 00007$
        ld  hl, #_pk_y
        sub a, (hl)
        cp  a, #161
        jp  nc, 00007$
        ld  c, a
        ; screen x
        pop hl
        push hl
        inc hl
        inc hl
        ld  a, (hl+)
        ld  b, (hl)
        srl b
        rra
        srl b
        rra
        ld  hl, #_pk_x
        sub a, (hl)
        ld  e, a
        inc hl
        ld  a, b
        sbc a, (hl)
        jp  nz, 00007$
        ld  a, e
        cp  a, #169
        jp  nc, 00007$
        ; fading out: blink
        pop hl
        push hl
        ld  a, (hl)
        cp  a, #6
        jr  nc, 00005$
        ld  a, (_pk_blink)
        or  a
        jr  nz, 00007$
    00005$:
        ld  hl, #_p_oam
        ld  a, (hl+)
        ld  h, (hl)
        ld  l, a
        ld  (hl), c
        inc hl
        ld  (hl), e
        jr  00008$
    00007$:
        ld  hl, #_p_oam
        ld  a, (hl+)
        ld  h, (hl)
        ld  l, a
        ld  (hl), #0
    00008$:
        ld  hl, #_p_oam
        ld  a, (hl)
        add a, #4
        ld  (hl+), a
        jr  nc, 00009$
        inc (hl)
    00009$:
        pop hl
        ld  de, #8
        add hl, de
        ld  a, (_p_count)
        dec a
        ld  (_p_count), a
        jp  nz, 00001$
        ret
    __endasm;
}

void part_update(void)
{
    /* (a DMG moves its particles at 30 Hz: the frame belongs to the game) */
    if (!is_cgb && (frame & 1)) return;
    pk_x = (uint16_t)(cam_x - 8 + shake_x);
    pk_y = (uint8_t)(cam_y - 16 + shake_y);
    pk_blink = (uint8_t)(frame & 2);
    part_asm();
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
    a_on = 1;
}

void anim_update(void)
{
    /* every timer runs; at most one animation is copied per frame (64 bytes) */
    uint8_t i, n, done = 0;
    if (!a_on) return;
    for (i = 0; i < NUM_ANIM; i++)
        if (a_timer[i] < 255) a_timer[i]++;
    if (tq_n) return;               /* the copy queue is busy: next frame */
    for (n = 0; n < NUM_ANIM && !done; n++) {
        i = a_cursor;
        if (++a_cursor == NUM_ANIM) a_cursor = 0;
        if (a_timer[i] < a_speed_r[i]) continue;
        if (i == ANIM_DOOR && a_door_off) continue;
        a_timer[i] = 0;
        if (++a_frame[i] >= a_frames_r[i]) a_frame[i] = 0;
        {
            ASSETS_IN();
            tq_push(a_first[i], anim_data[i] + ((uint16_t)a_frame[i] << 6));
            ASSETS_OUT();
        }
        done = 1;
    }
}

/* ------------------------------------------------------------ tile swaps */

/* Phase blocks are always drawn with their "on" tiles; when gravity turns, the art of
 * those tile slots is swapped, so every block on screen changes at once. */
static void copy_mt_art(uint8_t dst_mt, uint8_t src_mt)
{
    /* (both metatiles own 4 consecutive tiles) */
    ASSETS_IN();
    tq_push(mt_tiles[dst_mt * 4], art_tiles + ((uint16_t)(mt_tiles[src_mt * 4] - TILE_ART_BASE) << 4));
    ASSETS_OUT();
}

void gfx_phase(uint8_t g)
{
    copy_mt_art(MT_PHASE_P_ON, g ? MT_PHASE_P_OFF : MT_PHASE_P_ON);
    copy_mt_art(MT_PHASE_G_ON, g ? MT_PHASE_G_ON : MT_PHASE_G_OFF);
}

static uint8_t laser_shown = 1;
void gfx_laser(uint8_t on)
{
    if (on == laser_shown) return;
    laser_shown = on;
    a_door_off = (uint8_t)!on;
    copy_mt_art(MT_DOOR_CLOSED, on ? MT_DOOR_CLOSED : MT_DOOR_OPEN);
}

/* a boss's 32 tiles into sprite tiles BOSS_TILE_BASE.. (bank 0: it maps the art bank) */
void gfx_load_boss(uint8_t art)
{
    ASSETS_IN();
    set_sprite_data(BOSS_TILE_BASE, 32, boss_tiles[art]);
    ASSETS_OUT();
}

/* spr16a: a 16x16 sprite from four 8x8 tiles at OAM slot a: position spr_x / spr_y
 * (OAM coordinates), tiles spr_t + 0..3 (TL TR BL BR), attributes spr_p; its X / Y
 * flip bits also swap the tiles round. Asm: drawing is most of a DMG frame. */
uint8_t spr_x, spr_y, spr_t, spr_p;

void spr16a(uint8_t slot) __naked
{
    (void)slot;
    __asm
        ld  l, a
        ld  h, #0
        add hl, hl
        add hl, hl
        ld  de, #_shadow_OAM
        add hl, de
        ld  a, (_spr_p)
        ld  c, a
        ld  a, (_spr_t)
        ld  b, a
        ld  a, c
        and a, #0x60
        swap a
        rrca
        ld  e, a            ; tile xor mask: 1 = x flip, 2 = y flip
        ld  a, (_spr_x)
        ld  d, a
        ; top left
        ld  a, (_spr_y)
        ld  (hl+), a
        ld  a, d
        ld  (hl+), a
        ld  a, e
        add a, b
        ld  (hl+), a
        ld  a, c
        ld  (hl+), a
        ; top right
        ld  a, (_spr_y)
        ld  (hl+), a
        ld  a, d
        add a, #8
        ld  (hl+), a
        ld  a, e
        xor a, #1
        add a, b
        ld  (hl+), a
        ld  a, c
        ld  (hl+), a
        ; bottom left
        ld  a, (_spr_y)
        add a, #8
        ld  (hl+), a
        ld  a, d
        ld  (hl+), a
        ld  a, e
        xor a, #2
        add a, b
        ld  (hl+), a
        ld  a, c
        ld  (hl+), a
        ; bottom right
        ld  a, (_spr_y)
        add a, #8
        ld  (hl+), a
        ld  a, d
        add a, #8
        ld  (hl+), a
        ld  a, e
        xor a, #3
        add a, b
        ld  (hl+), a
        ld  (hl), c
        ret
    __endasm;
}

/* spr32a: a 32x32 picture (16 sprites, tiles spr_t + row * 4 + column, mirrored by the
 * flip bits of spr_p) at OAM slot a, top-left spr_x / spr_y (OAM coordinates); bit k of
 * spr_vis says sprite k (row k / 4, column k % 4) is on screen. */
uint16_t spr_vis;
static uint8_t spr_xm;

void spr32a(uint8_t slot) __naked
{
    (void)slot;
    __asm
        ld  l, a
        ld  h, #0
        add hl, hl
        add hl, hl
        ld  de, #_shadow_OAM
        add hl, de
        ; tile xor mask: x flip 3, y flip 12
        ld  a, (_spr_p)
        ld  c, #0
        bit 5, a
        jr  z, 00001$
        ld  c, #3
    00001$:
        bit 6, a
        jr  z, 00002$
        ld  a, c
        or  a, #12
        ld  c, a
    00002$:
        ld  a, c
        ld  (_spr_xm), a
        ld  a, (_spr_vis)
        ld  e, a
        ld  a, (_spr_vis + 1)
        ld  d, a
        ld  b, #0
    00003$:
        srl d
        rr  e
        jr  nc, 00004$
        ld  a, b
        and a, #0x0C
        add a, a
        ld  c, a
        ld  a, (_spr_y)
        add a, c
        ld  (hl+), a
        ld  a, b
        and a, #3
        add a, a
        add a, a
        add a, a
        ld  c, a
        ld  a, (_spr_x)
        add a, c
        ld  (hl+), a
        ld  a, (_spr_xm)
        xor a, b
        ld  c, a
        ld  a, (_spr_t)
        add a, c
        ld  (hl+), a
        ld  a, (_spr_p)
        ld  (hl+), a
        jr  00005$
    00004$:
        xor a, a
        ld  (hl+), a
        inc hl
        inc hl
        inc hl
    00005$:
        inc b
        ld  a, b
        cp  a, #16
        jr  nz, 00003$
        ret
    __endasm;
}
