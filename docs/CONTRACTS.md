# ALIGNMENT - contracts between the core, the front end, the art and the music

This file pins the names and layouts that different parts of the code base agree on.
Change a contract here first, then on both sides.

## 1. The world (core)

* A **stage** is one long corridor of 16x16 px cells, 16 cells tall (the full 256 px BG map
  height) and 96 to 128 cells wide (about 10 to 13 screens). Cell index is column-major:
  `SCELL(x, y) = x*16 + y`, so a column is 16 consecutive bytes.
* Gravity is **down** or **up** (Metal Storm). Down = purple polarity, up = green polarity.
  Phase blocks of the gravity's colour are solid, the other colour is a ghost.
* Stage *n* of seed *s* is always the same stage (`stage_gen(&S, s, n)`), on the host and on
  the Game Boy. The core physics (`mech_step`) is shared too, so the host's physics bot
  (`tests/physbot.c`) proves every stage can be finished with real button presses and the
  ROM tests replay those presses on the cartridge.
* Enemies run left to right in `Stage.enemy[]` (the game only scans the ones on screen) and
  move on the stage clock: everything that moves repeats every 512 frames.
* Positions are in 1/16 px; the mech's hitbox is `HB_W x HB_H` inside its 16x16 sprite.

## 2. BG tile memory (LCDC bit 4 = 0: BG/window tiles at 0x8800-0x97FF, signed)

Sprites use 0x8000-0x87FF (sprite tiles 0..127) **only**, so all 256 BG tile indices are
background-only:

| BG index | Count | What |
| --- | --- | --- |
| 0..63 | 64 | font, ASCII 32..95 (0 = space = blank, all colour 0) |
| 64..79 | 16 | UI tiles |
| 80..255 | 176 | stage art (walls, metatiles, animation slots); the title logo is loaded over this region on the title screen |

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
enum { UI_BOLT, UI_HULL, UI_HULL_EMPTY, UI_CELL, UI_SHIELD, UI_BOX_TL, UI_BOX_T, UI_BOX_TR, UI_BOX_L,
       UI_BOX_R, UI_BOX_BL, UI_BOX_B, UI_BOX_BR, UI_BAR_FULL, UI_BAR_HALF, UI_GRAV_DOWN,
       UI_GRAV_UP, ... };   /* values are TILE_UI_BASE + n; box edges may alias */

/* stage art: one block of tile data loaded at TILE_ART_BASE */
#define NUM_ART_TILES n          /* <= 176 */
extern const uint8_t art_tiles[NUM_ART_TILES*16];

/* walls are auto-tiled per 8x8 quarter: wall_tile[corner*WALL_VARIANTS + variant] */
enum { WC_TL, WC_TR, WC_BL, WC_BR };
enum { WV_FILL, WV_HEDGE, WV_VEDGE, WV_OUTER, WV_INNER, WV_FILL2, WALL_VARIANTS };
extern const uint8_t wall_tile[4*WALL_VARIANTS];   /* absolute BG indices */

/* metatiles: 4 absolute BG indices each (TL,TR,BL,BR) + a CGB palette */
enum { MT_EMPTY, MT_BACK1, MT_BACK2, MT_BACK3, MT_PLASMA, MT_CELL, MT_CRATE,
       MT_PHASE_P_ON, MT_PHASE_P_OFF, MT_PHASE_G_ON, MT_PHASE_G_OFF,
       MT_PLATE_F, MT_PLATE_F_ON, MT_DOOR_CLOSED, MT_DOOR_OPEN, MT_EXIT_R,
       MT_ITEM_POWER, MT_ITEM_SHIELD, MT_ITEM_BOMB, MT_ITEM_REPAIR,   /* pick-up capsules */
       MT_BOSS_DOOR,                                 /* solid blast shutter, stacks vertically */
       MT_DECO_PIPE, MT_DECO_PIPE_V, MT_DECO_WINDOW, MT_DECO_LIGHT,   /* non-solid background */
       MT_DECO_GRATE, NUM_MT };
extern const uint8_t mt_tiles[NUM_MT*4];
extern const uint8_t mt_cgb_pal[NUM_MT];
#define PAL_WALL ...                                /* CGB palette of wall quarters */

/* tile animations: frames of 4 tiles (64 bytes) copied over a metatile's 4 tiles */
enum { ANIM_PLASMA, ANIM_CELL, ANIM_EXIT_R, ANIM_DOOR, NUM_ANIM };
extern const uint8_t anim_mt[NUM_ANIM];             /* which metatile each animates */
extern const uint8_t anim_frames[NUM_ANIM];         /* frame count (1..8) */
extern const uint8_t anim_speed[NUM_ANIM];          /* frames per animation step */
extern const uint8_t *const anim_data[NUM_ANIM];    /* frame k = anim_data[a] + k*64 */

/* sprites (8x8 mode), tiles 0..127 at 0x8000; 16x16 frames are TL,TR,BL,BR */
#define NUM_SPR_TILES n          /* <= BOSS_TILE_BASE (96): tiles 96..127 hold boss art */
extern const uint8_t spr_tiles[NUM_SPR_TILES*16];
enum { SPR_MECH_IDLE, SPR_MECH_IDLE2, SPR_MECH_WALK1, SPR_MECH_WALK2, SPR_MECH_FALL,
       SPR_MECH_FLIP, SPR_MECH_LAND, SPR_MECH_DEAD,
       SPR_MECH_SHOOT,                                       /* 16x16, facing right, gravity down */
       SPR_BOT_A, SPR_BOT_B, SPR_BOT_WALK, SPR_BOT_DEAD,      /* 16x16 */
       SPR_CRATE,                                            /* 16x16, same art as MT_CRATE */
       SPR_SPARK_BIG, SPR_SPARK_SMALL, SPR_PIXEL, SPR_STREAK, SPR_STAR,
       SPR_CURSOR, SPR_RING,                                 /* 8x8 (RING = TL quarter) */
       SPR_SHOT, SPR_SHOT_POWER, SPR_BOLT,                   /* 8x8 projectiles */
       SPR_TURRET, SPR_TURRET_FIRE,                          /* 16x16, floor-mounted, barrel right */
       SPR_EXPLODE1, SPR_EXPLODE2, ... };                    /* 16x16 */

/* boss art: the game copies one boss (32 tiles) into sprite tiles 96..127 */
enum { BOSS_SENTINEL, BOSS_PISTON, BOSS_HIVE, NUM_BOSS_ART };
#define BOSS_TILE_BASE 96
extern const uint8_t boss_tiles[NUM_BOSS_ART][32*16];  /* frame f, tile k (4x4 row-major in the
                                                         32x32 frame) = tile f*16+k */

/* title logo: loaded at TILE_ART_BASE, map holds absolute indices */
#define LOGO_W, LOGO_H, LOGO_NTILES
extern const uint8_t logo_tiles[], logo_map[], logo_cgb_attr[];

/* CGB palettes (RGB555) + palette name defines, DMG BGP/OBP0/OBP1 */
extern const uint16_t bg_cgb_pal[8*4], spr_cgb_pal[8*4];
```

Mech frames are drawn **facing right with gravity down**; the game mirrors them with the OAM
X flip (facing left) and Y flip (gravity up). The mech is 16x16 and stands on the bottom row
of its cell; its arm cannon muzzle is at x=15, y=7..8 of the frame (spawn shots there).

Orientation / palette notes for the new art:

* `SPR_SHOT`, `SPR_SHOT_POWER` travel right (X flip = left); SHOT occupies rows 2..5 (core rows
  3..4), SHOT_POWER rows 0..7. Suggested palettes: `SPAL_SPARK_P` / `SPAL_SPARK_G` (white core).
* `SPR_BOLT` (enemy shot) is a round orb: use `SPAL_DEAD` (white core, magenta ring).
* `SPR_TURRET` / `SPR_TURRET_FIRE`: base on row 15, barrel right (muzzle x=15, y=6..7):
  X flip = facing left, Y flip = ceiling-mounted. Palette `SPAL_BOT` (DMG: OBP1).
* `SPR_EXPLODE1` (fireball) then `SPR_EXPLODE2` (debris ring): `SPAL_DEAD` or `SPAL_SPARK_P`.
* Bosses use `SPAL_BOT` (DMG: OBP1). `BOSS_SENTINEL` faces **left** (X flip = right); frame 1 is
  the other walking step. `BOSS_PISTON`: rod at the top, striking face (teeth) on rows 28..31;
  frame 0 shutter closed, frame 1 open with the eye (weak point, centre x=16, y=17) exposed.
  `BOSS_HIVE`: frame 0 calm, frame 1 pulsing (vents open). Each frame is 4x4 hardware sprites.
* Items (`MT_ITEM_*`) are glowing capsules with their own CGB palette: POWER 'P' = PAL_HAZARD,
  SHIELD = PAL_GREEN, BOMB (up/down arrows) = PAL_PURPLE, REPAIR (plus) = PAL_UI.
  `MT_BOSS_DOOR` = PAL_HAZARD, solid. `MT_DECO_*` = PAL_METAL colour 1 only (dark grey on DMG),
  non-solid background; PIPE tiles horizontally, PIPE_V vertically.
* HUD: `UI_HULL` / `UI_HULL_EMPTY` are one pip each (draw N side by side), `UI_SHIELD` an icon.

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
