/* ALIGNMENT - Game Boy drawing helpers, palettes, effects and the VBlank handler. */
#ifndef AL_GFX_H
#define AL_GFX_H
#include <stdint.h>

extern uint8_t is_cgb;
extern volatile uint8_t frame;      /* incremented in VBlank */
extern uint8_t joy, joy_prev, joy_new;

/* sprite slots (8x8 OAM mode) */
#define SP_MECH   0     /* 4 */
#define SP_SHOT   4     /* the mech's shots, 3 */
#define SP_BOLT   7     /* turret and boss bolts, 4 */
#define NUM_BOLT  4
#define SP_ENEMY  11    /* 4 enemies (or explosions) x 4; a boss takes all 16 */
#define NUM_ESPR  4
#define SP_ROD    27    /* a crusher's piston rod, 3 */
#define NUM_ROD   3
#define SP_PART   30    /* particles */
#define NUM_PART  10

#define HUD_WY 136      /* the HUD is one window row at the bottom of the screen */

void gfx_init(void);
void gfx_load_play_tiles(void);
void gfx_load_title_tiles(void);
void gfx_load_boss(uint8_t art);
/* a 16x16 sprite at OAM slot `slot` (see gfx.c) */
extern uint8_t spr_x, spr_y, spr_t, spr_p;
void spr16a(uint8_t slot);
extern uint16_t spr_vis;
void spr32a(uint8_t slot);
void gfx_hide_sprites(void);
void gfx_draw_logo(uint8_t y0);
void input_poll(void);
void wait_frames(uint8_t n);

/* BG tile data with LCDC bit 4 clear: index i < 128 at 0x9000, i >= 128 at 0x8800 */
void bg_tile_data(uint8_t first, uint8_t n, const uint8_t *src);

/* text on the BG map (win = 0) or the window (win = 1); pal is the CGB palette */
void txt(uint8_t win, uint8_t x, uint8_t y, const char *s, uint8_t pal);
void txt_num(uint8_t win, uint8_t x, uint8_t y, uint16_t v, uint8_t digits, uint8_t pal);
/* a score, 7 digits (up to 9 999 999) */
void txt_score(uint8_t win, uint8_t x, uint8_t y, uint32_t sc, uint8_t pal);
void txt_hex(uint8_t win, uint8_t x, uint8_t y, uint16_t v, uint8_t pal);
/* the HUD: window row 0, edited here and copied in the next VBlank by hud_commit() */
extern uint8_t hud_t[20], hud_a[20];
void hud_commit(void);
void put_tile(uint8_t win, uint8_t x, uint8_t y, uint8_t tile, uint8_t pal);
void gfill(uint8_t win, uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t tile, uint8_t pal);
void draw_box(uint8_t win, uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t pal);
void clear_bkg(void);
void clear_win(void);
/* a w x h block of tiles + CGB attributes at map (x, y) (wrapping) */
void put_block(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint8_t *tiles, const uint8_t *attrs);

/* palettes */
void pal_set_deck(uint8_t deck);     /* CGB: the hull palette of a deck */
void pal_gravity(uint8_t g);         /* the mech's polarity colours */
void pal_apply(void);                /* (re)write every palette, honouring the fade level */
void fade_to(uint8_t level);         /* 0 = full colour .. 4 = black, animated */
void flash(uint8_t frames, uint8_t white);   /* palette flash (white or polarity colour) */

/* screen shake and scroll */
extern uint8_t scroll_x, scroll_y;  /* the camera (BG scroll) */
extern int8_t shake_x, shake_y;
void fx_shake(uint8_t amount);
void fx_update(void);               /* once per frame: shake, flash, tile animations */

/* particles */
void part_clear(void);
/* world pixel coordinates; speeds in 1/4 px a frame */
extern uint16_t cam_x;
extern uint8_t cam_y;
void part_burst(uint16_t px, uint8_t py, uint8_t n, uint8_t tile, uint8_t pal, uint8_t speed, int8_t gravity);
void part_spawn(uint16_t px, uint8_t py, int8_t vx, int8_t vy, uint8_t tile, uint8_t pal, uint8_t life);
void part_update(void);

/* stage streaming: stage column cx (prebuilt in cartridge RAM) copied to the BG map
 * in VBlank, while stream_on */
extern volatile uint8_t stream_on;
void col_push(uint8_t cx);
uint8_t col_queued(void);
/* phase blocks show the gravity g; laser gates on / off */
void gfx_phase(uint8_t g);
void gfx_laser(uint8_t on);

/* tile animations of the room art (plasma, cells, fields, exits, doors) */
void anim_reset(void);
void anim_update(void);
uint8_t fx_rand(void);              /* visual randomness (never the game's rng) */

#endif
