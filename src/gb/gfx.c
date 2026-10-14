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
    memcpy(tq_dst[tq_head], tq_src[tq_head], 64);
    SWITCH_ROM(b);
    tq_head = (uint8_t)((tq_head + 1) & (TQ - 1));
    tq_n--;
}

/* ------------------------------------------------------------ VBlank */

static void vbl_isr(void)
{
    /* a DMG's VBlank fits one column or one art copy; a Color's twice that */
    if (cq_n && stream_on) {
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

uint8_t dbg_fx[3];
void fx_update(void)
{
    dbg_fx[0] = LY_REG;
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
    dbg_fx[1] = LY_REG;
    anim_update();
    dbg_fx[2] = LY_REG;
}

/* ------------------------------------------------------------ particles */

/* Particles live in world pixels and are drawn relative to the camera. Speeds are in
 * 1/4 px a frame, kept in a 2-bit fraction; all the per-frame maths is 8-bit and the
 * sprites are written straight into the shadow OAM. A DMG moves half of them each frame
 * (twice as far): it can't afford all of them. */
uint16_t cam_x;
uint8_t cam_y;
typedef struct {
    uint8_t life;
    uint8_t y;
    uint16_t x;
    int8_t vx, vy, g;
    uint8_t f;          /* x fraction | y fraction << 2 */
} Part;
static Part part[NUM_PART];
static uint8_t p_next;

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
    p->x = px;
    p->y = py;
    p->f = 0;
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
    uint8_t mask = 1;
    if (!is_cgb) n = (uint8_t)((n + 1) >> 1);
    while (mask < speed) mask = (uint8_t)((mask << 1) | 1);    /* speed..2*speed, a power of two - 1 */
    while (n--) {
        Part *p = &part[p_next];
        part_spawn(px, py, (int8_t)((int8_t)(fx_rand() & mask) - (int8_t)(mask >> 1)),
                   (int8_t)((int8_t)(fx_rand() & mask) - (int8_t)(mask >> 1)),
                   tile, pal, (uint8_t)(14 + (fx_rand() & 15)));
        p->g = gravity;
    }
}

void part_update(void)
{
    uint8_t n, sy, step, fx, fy, life;
    int8_t t, vx, vy;
    uint16_t sx;
    Part *p;
    OAM_item_t *o;
    step = is_cgb ? 1 : 2;
    n = is_cgb ? 0 : (uint8_t)(frame & 1);
    p = &part[n];
    o = &shadow_OAM[SP_PART + n];
    for (; n < NUM_PART; n += step, p += step, o += step) {
        life = p->life;
        if (!life) continue;
        if (life <= step) {
            p->life = 0;
            o->y = 0;
            continue;
        }
        p->life = life = (uint8_t)(life - step);
        vx = p->vx;
        vy = p->vy;
        if (step == 2) { vx = (int8_t)(vx << 1); vy = (int8_t)(vy << 1); }
        fx = (uint8_t)(p->f & 3);
        fy = (uint8_t)(p->f >> 2);
        t = (int8_t)(fx + vx);
        p->x += (int8_t)(t >> 2);
        fx = (uint8_t)(t & 3);
        t = (int8_t)(fy + vy);
        p->y = (uint8_t)(p->y + (int8_t)(t >> 2));
        p->f = (uint8_t)(fx | ((t & 3) << 2));
        if (p->g) p->vy = (int8_t)(p->vy + p->g);
        sx = (uint16_t)(p->x - cam_x + 8);
        sy = (uint8_t)(p->y - cam_y + 16);
        if ((life < 6 && (frame & 2)) || sx > 168 || sy > 160) {
            o->y = 0;
        } else {
            o->y = (uint8_t)(sy - shake_y);
            o->x = (uint8_t)((uint8_t)sx - shake_x);
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
