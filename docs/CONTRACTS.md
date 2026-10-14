# ALIGNMENT - contracts between the core, the front end, the art and the music

This file pins the names and layouts that different parts of the code base agree on.
Change a contract here first, then on both sides.

## 1. The world (core)

* A **room** is 10 x 9 cells of 16x16 px: the whole 160x144 screen. Cell index = `y*10 + x`
  (0..89). Row 0 and row 8, column 0 and column 9 are the **border**: always wall, except
  the room's two **openings** (entry and exit). Corner cells are always wall.
* Gravity is **down** or **up** (Metal Storm). Down = purple polarity, up = green polarity.
* Rooms are chained: room n's exit side decides room n+1's entry side (right exit -> left
  entry, same row; up exit -> bottom entry, same column, gravity up; down exit -> top entry,
  same column, gravity down). Exits are never on the left, so the route never crosses itself.

## 2. BG tile memory (LCDC bit 4 = 0: BG/window tiles at 0x8800-0x97FF, signed)

Sprites use 0x8000-0x87FF (sprite tiles 0..127) **only**, so all 256 BG tile indices are
background-only:

| BG index | Count | What |
| --- | --- | --- |
| 0..63 | 64 | font, ASCII 32..95 (0 = space = blank, all colour 0) |
| 64..79 | 16 | UI tiles |
| 80..255 | 176 | room art (walls, metatiles, animation slots); the title logo is loaded over this region on the title screen |

Tile index *i* lives at `0x9000 + i*16` for i < 128 and at `0x8800 + (i-128)*16` for i >= 128.

## 3. The generated asset header (`src/gb/assets.h`, written by `tools/gen_assets.py`)

```c
#define TILE_FONT_BASE 0
#define TILE_UI_BASE   64
#define TILE_ART_BASE  80
#define FONT_TILE(ch)  ((uint8_t)((ch) - 32))      /* ASCII 32..95 */

extern const uint8_t font_tiles[64*16];
extern const uint8_t ui_tiles[16*16];

/* UI tiles: absolute BG indices */
enum { UI_BOLT, UI_STEP, UI_STAR, UI_CELL, UI_FLAG, UI_BOX_TL, UI_BOX_T, UI_BOX_TR, UI_BOX_L,
       UI_BOX_R, UI_BOX_BL, UI_BOX_B, UI_BOX_BR, UI_BAR_FULL, UI_BAR_HALF, UI_GRAV_DOWN,
       UI_GRAV_UP, ... };   /* values are TILE_UI_BASE + n; box edges may alias */

/* room art: one block of tile data loaded at TILE_ART_BASE */
#define NUM_ART_TILES n          /* <= 176 */
extern const uint8_t art_tiles[NUM_ART_TILES*16];

/* walls are auto-tiled per 8x8 quarter: wall_tile[corner*WALL_VARIANTS + variant] */
enum { WC_TL, WC_TR, WC_BL, WC_BR };
enum { WV_FILL, WV_HEDGE, WV_VEDGE, WV_OUTER, WV_INNER, WV_FILL2, WALL_VARIANTS };
extern const uint8_t wall_tile[4*WALL_VARIANTS];   /* absolute BG indices */

/* metatiles: 4 absolute BG indices each (TL,TR,BL,BR) + a CGB palette */
enum { MT_EMPTY, MT_BACK1, MT_BACK2, MT_BACK3, MT_PLASMA, MT_CELL, MT_CRATE,
       MT_PHASE_P_ON, MT_PHASE_P_OFF, MT_PHASE_G_ON, MT_PHASE_G_OFF,
       MT_PLATE_F, MT_PLATE_F_ON, MT_PLATE_C, MT_PLATE_C_ON, MT_DOOR_CLOSED, MT_DOOR_OPEN,
       MT_FIELD_V, MT_FIELD_H, MT_EXIT_R, MT_EXIT_U, MT_EXIT_D,
       MT_HATCH_L, MT_HATCH_T, MT_HATCH_B, NUM_MT };
extern const uint8_t mt_tiles[NUM_MT*4];
extern const uint8_t mt_cgb_pal[NUM_MT];
#define PAL_WALL ...                                /* CGB palette of wall quarters */

/* tile animations: frames of 4 tiles (64 bytes) copied over a metatile's 4 tiles */
enum { ANIM_PLASMA, ANIM_CELL, ANIM_FIELD_V, ANIM_FIELD_H, ANIM_EXIT_R, ANIM_EXIT_U,
       ANIM_EXIT_D, ANIM_DOOR, NUM_ANIM };
extern const uint8_t anim_mt[NUM_ANIM];             /* which metatile each animates */
extern const uint8_t anim_frames[NUM_ANIM];         /* frame count (1..8) */
extern const uint8_t anim_speed[NUM_ANIM];          /* frames per animation step */
extern const uint8_t *const anim_data[NUM_ANIM];    /* frame k = anim_data[a] + k*64 */

/* sprites (8x8 mode), tiles 0..127 at 0x8000; 16x16 frames are TL,TR,BL,BR */
#define NUM_SPR_TILES n
extern const uint8_t spr_tiles[NUM_SPR_TILES*16];
enum { SPR_MECH_IDLE, SPR_MECH_IDLE2, SPR_MECH_WALK1, SPR_MECH_WALK2, SPR_MECH_FALL,
       SPR_MECH_FLIP, SPR_MECH_LAND, SPR_MECH_DEAD,          /* 16x16, facing right, gravity down */
       SPR_BOT_A, SPR_BOT_B, SPR_BOT_WALK, SPR_BOT_DEAD,      /* 16x16 */
       SPR_CRATE,                                            /* 16x16, same art as MT_CRATE */
       SPR_SPARK_BIG, SPR_SPARK_SMALL, SPR_PIXEL, SPR_STREAK, SPR_STAR,
       SPR_ARROW_R, SPR_ARROW_U, SPR_CURSOR, SPR_FLIP_ICON, ... };  /* 8x8 */

/* title logo: loaded at TILE_ART_BASE, map holds absolute indices */
#define LOGO_W, LOGO_H, LOGO_NTILES
extern const uint8_t logo_tiles[], logo_map[], logo_cgb_attr[];

/* CGB palettes (RGB555) + palette name defines, DMG BGP/OBP0/OBP1 */
extern const uint16_t bg_cgb_pal[8*4], spr_cgb_pal[8*4];
```

Mech frames are drawn **facing right with gravity down**; the game mirrors them with the OAM
X flip (facing left) and Y flip (gravity up). The mech is 16x16 and stands on the bottom row
of its cell.

## 4. Sound (`src/gb/sound.h`)

```c
enum { SONG_TITLE, SONG_DECK_A, SONG_DECK_B, SONG_DECK_C, SONG_ZEN, SONG_GAMEOVER, NUM_SONGS };
enum { SFX_STEP, SFX_BUMP, SFX_FLIP, SFX_LAND, SFX_PUSH, SFX_CRATE_LAND, SFX_CELL,
       SFX_EXIT_OPEN, SFX_EXIT, SFX_ZAP, SFX_CRUSH, SFX_BOT_STEP, SFX_BOT_DIE, SFX_DOOR,
       SFX_REWIND, SFX_MENU, SFX_SELECT, SFX_OPTIMAL, SFX_NEWMECH, SFX_HINT, SFX_ERROR,
       SFX_SCAN, SFX_DECK, NUM_SFX };
```

The driver API is the Infinite Best one: `sound_init`, `sound_tick` (from VBlank),
`music_play`, `music_stop`, `music_playing`, `sfx_play`, `music_set_rewind`,
`music_set_muffle`, `sound_mute_all`, plus the debug helpers.
