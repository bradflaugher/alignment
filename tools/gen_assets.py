#!/usr/bin/env python3
"""ALIGNMENT asset generator.

Parses the human-editable ASCII-art sources in ``assets/`` and writes
``src/gb/assets.c`` + ``src/gb/assets.h`` (GBDK-2020 / SDCC friendly).
The generated API is pinned by docs/CONTRACTS.md sections 2 and 3.

Stdlib only, fully deterministic (same input -> byte-identical output).

    python3 tools/gen_assets.py           # (re)write src/gb/assets.[ch]
    python3 tools/gen_assets.py --check   # exit 1 if they are out of date

Source format (all files in assets/*.txt; assets/music is not read)
-------------------------------------------------------------------
* Lines starting with ``#`` (in column 0) are comments; blank lines end a block.
* A block starts with a header line ``@<kind> <NAME...> [key=value ...]``
  followed by rows of pixels.  Pixel characters:
      ``.`` = colour index 0 (black background / transparent for sprites)
      ``1`` = index 1 (dim)   ``2`` = index 2 (bright)   ``3`` = index 3 (hot)
  Block width/height are taken from the rows and must be multiples of 8.
  Multi-tile blocks are split into 8x8 tiles in row-major order, so a 16x16
  block becomes TL, TR, BL, BR.
* Kinds:
    ``@char X``        font glyph for character X (``@char SPACE`` for ' ').
    ``@tile NAME``     UI tile or sprite tile(s).  A multi-tile block may name
                       every tile (``@tile A B``) or just the first one.
                       ``A|B`` gives one tile two enum names (B aliases A).
    ``@wall VARIANT``  16x16 wall cell drawn with that variant in all four
                       quarters (FILL, HEDGE, VEDGE, OUTER, INNER, FILL2):
                       its TL quarter is wall_tile[WC_TL*6+WV_x], etc.
    ``@meta NAME pal=PALNAME``   static 16x16 metatile.
    ``@anim ANIM_X mt=MT_Y pal=PALNAME speed=N``  animated metatile: a strip
                       of N frames of 16x16 side by side.  Frame 0 is also the
                       metatile's resting art; its 4 tiles are never shared.
    ``@boss BOSS_X``   boss sprite art: a 64x32 canvas holding two 32x32 frames
                       side by side.  Frame f is split into 16 tiles (4x4,
                       row-major) stored as boss_tiles[BOSS_X][(f*16+k)*16].
                       Drawn with the SPAL_BOT colours; loaded by the game into
                       sprite tiles BOSS_TILE_BASE..127.
    ``@logo``          the title logo pixel canvas (width = LOGO_W*8).
    ``@logo_attr``     one char per logo cell: CGB BG palette number 0-7.
    ``@bgpal N NAME c0 c1 c2 c3`` / ``@sprpal ...`` / ``@deckpal ...``
                       palettes (single line, ``#rrggbb`` colours).
    ``@alias NAME TARGET``  extra ``#define NAME TARGET`` in the header.
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

TILE_FONT_BASE = 0
TILE_UI_BASE = 64
TILE_ART_BASE = 80
NUM_FONT = 64
NUM_UI = 16
MAX_ART = 176
MAX_SPR = 128
BOSS_TILE_BASE = 96     # sprite tiles 96..127 are reserved for one boss's art
MAX_SPR_ART = BOSS_TILE_BASE
BOSS_TILES = 32         # 2 frames x 16 tiles (32x32 each)
MAX_LOGO_TILES = 176
MAX_ANIM_FRAMES = 8

PIX = {'.': 0, '1': 1, '2': 2, '3': 3}

WALL_CORNERS = ['WC_TL', 'WC_TR', 'WC_BL', 'WC_BR']
WALL_VARIANTS = ['FILL', 'HEDGE', 'VEDGE', 'OUTER', 'INNER', 'FILL2']

MT_ORDER = [
    'MT_EMPTY', 'MT_BACK1', 'MT_BACK2', 'MT_BACK3', 'MT_PLASMA', 'MT_CELL',
    'MT_CRATE', 'MT_PHASE_P_ON', 'MT_PHASE_P_OFF', 'MT_PHASE_G_ON',
    'MT_PHASE_G_OFF', 'MT_PLATE_F', 'MT_PLATE_F_ON', 'MT_DOOR_CLOSED',
    'MT_DOOR_OPEN', 'MT_EXIT_R',
    'MT_ITEM_POWER', 'MT_ITEM_SHIELD', 'MT_ITEM_BOMB', 'MT_ITEM_REPAIR',
    'MT_BOSS_DOOR',
    'MT_DECO_PIPE', 'MT_DECO_PIPE_V', 'MT_DECO_WINDOW', 'MT_DECO_LIGHT',
    'MT_DECO_GRATE',
]

ANIM_ORDER = [
    'ANIM_PLASMA', 'ANIM_CELL', 'ANIM_EXIT_R', 'ANIM_DOOR',
]

BOSS_ORDER = ['BOSS_SENTINEL', 'BOSS_PISTON', 'BOSS_HIVE']

UI_REQUIRED = [
    'UI_BOLT', 'UI_HULL', 'UI_HULL_EMPTY', 'UI_CELL', 'UI_SHIELD',
    'UI_BOX_TL', 'UI_BOX_T', 'UI_BOX_TR', 'UI_BOX_L', 'UI_BOX_R',
    'UI_BOX_BL', 'UI_BOX_B', 'UI_BOX_BR', 'UI_BAR_FULL', 'UI_BAR_HALF',
    'UI_GRAV_DOWN', 'UI_GRAV_UP',
]

SPR_REQUIRED = [
    'SPR_MECH_IDLE', 'SPR_MECH_IDLE2', 'SPR_MECH_WALK1', 'SPR_MECH_WALK2',
    'SPR_MECH_FALL', 'SPR_MECH_FLIP', 'SPR_MECH_LAND', 'SPR_MECH_DEAD',
    'SPR_MECH_SHOOT', 'SPR_BOT_A', 'SPR_BOT_B', 'SPR_BOT_WALK', 'SPR_BOT_DEAD', 'SPR_CRATE',
    'SPR_SPARK_BIG', 'SPR_SPARK_SMALL', 'SPR_PIXEL', 'SPR_STREAK', 'SPR_STAR',
    'SPR_CURSOR', 'SPR_RING', 'SPR_SHOT', 'SPR_SHOT_POWER', 'SPR_BOLT',
    'SPR_TURRET', 'SPR_TURRET_FIRE', 'SPR_EXPLODE1',
    'SPR_EXPLODE2', 'SPR_MECH_SHOOT_UP', 'SPR_SHOT_UP', 'SPR_SHOT_POWER_UP',
]
# 16x16 sprites (4 tiles TL,TR,BL,BR); everything else is a single 8x8 tile
SPR_16X16_PREFIXES = ('SPR_MECH', 'SPR_BOT', 'SPR_CRATE', 'SPR_TURRET',
                      'SPR_EXPLODE')

BG_PAL_REQUIRED = ['PAL_UI', 'PAL_WALL', 'PAL_GREEN', 'PAL_PURPLE',
                   'PAL_HAZARD', 'PAL_DIM']
SPR_PAL_REQUIRED = ['SPAL_MECH_P', 'SPAL_MECH_G', 'SPAL_BOT', 'SPAL_CRATE',
                    'SPAL_SPARK_G', 'SPAL_SPARK_P', 'SPAL_DEAD', 'SPAL_WHITE']
MIN_DECK_PALS = 4

# DMG palettes (index 0 = black: inverted like Infinite Best).
DMG_BGP = 0x1B    # 0 black, 1 dark grey, 2 light grey, 3 white
DMG_OBP0 = 0x1B   # same ramp as the BG
DMG_OBP1 = 0x1C   # 1 black, 2 light grey, 3 white: drones (crisp black outline)


class AssetError(Exception):
    pass


# --------------------------------------------------------------------------
# Parsing
# --------------------------------------------------------------------------

class Block(object):
    def __init__(self, kind, args, opts, rows, where):
        self.kind = kind
        self.args = args
        self.opts = opts
        self.rows = rows
        self.where = where

    @property
    def w(self):
        return len(self.rows[0]) if self.rows else 0

    @property
    def h(self):
        return len(self.rows)


SINGLE_LINE = ('bgpal', 'sprpal', 'deckpal', 'alias')


def parse_file(path):
    blocks = []
    cur = None
    with open(path, 'r') as f:
        lines = f.read().split('\n')
    for ln, raw in enumerate(lines, 1):
        line = raw.rstrip()
        where = '%s:%d' % (os.path.basename(path), ln)
        if line.startswith('#'):
            continue
        if not line.strip():
            cur = None
            continue
        if line.startswith('@'):
            toks = line[1:].split()
            kind = toks[0]
            args, opts = [], {}
            for t in toks[1:]:
                if kind == 'char':
                    args.append(t)  # '@char =' is a glyph, not an option
                elif '=' in t and not t.startswith('#'):
                    k, v = t.split('=', 1)
                    opts[k] = v
                else:
                    args.append(t)
            cur = Block(kind, args, opts, [], where)
            blocks.append(cur)
            if kind in SINGLE_LINE:
                cur = None
            continue
        if cur is None:
            raise AssetError('%s: pixel row outside of a block' % where)
        row = line.strip()
        for ch in row:
            if cur.kind != 'logo_attr' and ch not in PIX:
                raise AssetError('%s: bad pixel char %r' % (where, ch))
        if cur.rows and len(row) != len(cur.rows[0]):
            raise AssetError('%s: row width %d != %d' %
                             (where, len(row), len(cur.rows[0])))
        cur.rows.append(row)
    return blocks


def split_tiles(block):
    """Split a block into 8x8 tiles (row-major). Each tile = list of 8 rows
    of 8 ints."""
    if block.w % 8 or block.h % 8 or not block.rows:
        raise AssetError('%s: block size %dx%d not a multiple of 8' %
                         (block.where, block.w, block.h))
    tiles = []
    for ty in range(block.h // 8):
        for tx in range(block.w // 8):
            t = []
            for y in range(8):
                r = block.rows[ty * 8 + y][tx * 8:tx * 8 + 8]
                t.append([PIX[c] for c in r])
            tiles.append(t)
    return tiles


def split_frames(block):
    """A strip of 16x16 frames -> list of [TL, TR, BL, BR] tile lists."""
    if block.h != 16 or block.w % 16 or not block.w:
        raise AssetError('%s: animation strip must be 16 px tall, 16*n wide' %
                         block.where)
    frames = []
    for f in range(block.w // 16):
        sub = Block('tile', [], {}, [r[f * 16:f * 16 + 16] for r in block.rows],
                    block.where)
        frames.append(split_tiles(sub))
    return frames


def encode_tile(tile):
    out = []
    for row in tile:
        lo = hi = 0
        for x, v in enumerate(row):
            bit = 7 - x
            lo |= (v & 1) << bit
            hi |= ((v >> 1) & 1) << bit
        out.append(lo)
        out.append(hi)
    return out


def tkey(tile):
    return tuple(encode_tile(tile))


BLANK = [[0] * 8 for _ in range(8)]


def parse_color(s, where):
    s = s.lstrip('#')
    if len(s) != 6:
        raise AssetError('%s: bad colour %r' % (where, s))
    return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))


def rgb555(c):
    r, g, b = c
    return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)


def asset_files(assets_dir):
    return [os.path.join(assets_dir, n) for n in sorted(os.listdir(assets_dir))
            if n.endswith('.txt')]


def load_assets(assets_dir, strict=True):
    """Parse every assets/*.txt file and return a dict with everything the
    C writer and the previewer need."""
    blocks = []
    for p in asset_files(assets_dir):
        blocks.extend(parse_file(p))

    font = {}
    ui = []          # list of (name, tile)
    walls = {}       # variant -> [TL, TR, BL, BR]
    metas = {}       # name -> (tiles[4], palname)
    anims = {}       # name -> (mt, palname, speed, frames)
    spr = []         # list of (name, tile)
    boss = {}        # name -> 32 tiles (frame 0 tiles 0..15, frame 1 16..31)
    logo_block = None
    attr_block = None
    pals = {'bgpal': {}, 'sprpal': {}, 'deckpal': {}}
    aliases = []

    for b in blocks:
        k = b.kind
        if k in pals:
            if len(b.args) != 6:
                raise AssetError('%s: palette needs N NAME c0 c1 c2 c3' %
                                 b.where)
            n = int(b.args[0])
            cols = [parse_color(c, b.where) for c in b.args[2:]]
            dest = pals[k]
            if n in dest or n < 0 or (k != 'deckpal' and n > 7):
                raise AssetError('%s: bad/duplicate palette %d' % (b.where, n))
            dest[n] = (b.args[1], cols)
        elif k == 'alias':
            if len(b.args) != 2:
                raise AssetError('%s: @alias NAME TARGET' % b.where)
            aliases.append((b.args[0], b.args[1]))
        elif k == 'char':
            tok = b.args[0]
            ch = ' ' if tok == 'SPACE' else tok
            if len(ch) != 1 or not 32 <= ord(ch) <= 95:
                raise AssetError('%s: bad char %r' % (b.where, tok))
            if ch in font:
                raise AssetError('%s: duplicate char %r' % (b.where, ch))
            t = split_tiles(b)
            if len(t) != 1:
                raise AssetError('%s: glyph must be 8x8' % b.where)
            font[ch] = t[0]
        elif k == 'tile':
            tiles = split_tiles(b)
            names = list(b.args)
            if not names:
                raise AssetError('%s: @tile needs a name' % b.where)
            first = names[0]
            if len(names) == 1:
                names = names + [None] * (len(tiles) - 1)
            if len(names) != len(tiles):
                raise AssetError('%s: %d names for %d tiles' %
                                 (b.where, len(names), len(tiles)))
            if first.startswith('UI_'):
                dest = ui
            elif first.startswith('SPR_'):
                dest = spr
            else:
                raise AssetError('%s: unknown tile section for %s' %
                                 (b.where, first))
            dest.extend(zip(names, tiles))
        elif k == 'wall':
            v = b.args[0] if b.args else ''
            if v not in WALL_VARIANTS or v in walls:
                raise AssetError('%s: bad/duplicate wall variant %r' %
                                 (b.where, v))
            tiles = split_tiles(b)
            if len(tiles) != 4:
                raise AssetError('%s: wall block must be 16x16' % b.where)
            walls[v] = tiles
        elif k == 'meta':
            name = b.args[0]
            if name in metas:
                raise AssetError('%s: duplicate %s' % (b.where, name))
            tiles = split_tiles(b)
            if len(tiles) != 4:
                raise AssetError('%s: metatile must be 16x16' % b.where)
            pal = b.opts.get('pal')
            if pal is None:
                raise AssetError('%s: metatile needs pal=' % b.where)
            metas[name] = (tiles, pal)
        elif k == 'anim':
            name = b.args[0]
            if name in anims or name not in ANIM_ORDER:
                raise AssetError('%s: bad/duplicate animation %s' %
                                 (b.where, name))
            try:
                mt, pal, speed = b.opts['mt'], b.opts['pal'], int(b.opts['speed'])
            except (KeyError, ValueError):
                raise AssetError('%s: @anim needs mt= pal= speed=' % b.where)
            frames = split_frames(b)
            if not 1 <= len(frames) <= MAX_ANIM_FRAMES:
                raise AssetError('%s: 1..%d frames' % (b.where, MAX_ANIM_FRAMES))
            if not 1 <= speed <= 255:
                raise AssetError('%s: bad speed' % b.where)
            anims[name] = (mt, pal, speed, frames)
        elif k == 'boss':
            name = b.args[0] if b.args else ''
            if name not in BOSS_ORDER or name in boss:
                raise AssetError('%s: bad/duplicate boss %r' % (b.where, name))
            if b.w != 64 or b.h != 32:
                raise AssetError('%s: @boss must be 64x32 (two 32x32 frames)' %
                                 b.where)
            tiles = []
            for f in range(2):
                sub = Block('tile', [], {}, [r[f * 32:f * 32 + 32] for r in b.rows],
                            b.where)
                tiles.extend(split_tiles(sub))
            boss[name] = tiles
        elif k == 'logo':
            logo_block = b
        elif k == 'logo_attr':
            attr_block = b
        else:
            raise AssetError('%s: unknown block kind @%s' % (b.where, k))

    # ---- palettes ---------------------------------------------------------
    for kind in ('bgpal', 'sprpal'):
        for n in range(8):
            if n not in pals[kind]:
                raise AssetError('missing %s %d' % (kind, n))
    bgpal = [pals['bgpal'][n] for n in range(8)]
    sprpal = [pals['sprpal'][n] for n in range(8)]
    nd = len(pals['deckpal'])
    if nd < MIN_DECK_PALS or sorted(pals['deckpal']) != list(range(nd)):
        raise AssetError('need deck palettes 0..n-1 (n >= %d)' % MIN_DECK_PALS)
    deckpal = [pals['deckpal'][n] for n in range(nd)]
    black = bgpal[0][1][0]
    if black != (0, 0, 0):
        raise AssetError('BG colour 0 must be black')
    for name, cols in bgpal + deckpal:
        if cols[0] != black:
            raise AssetError('palette %s colour 0 is not black' % name)
    for names, pl in ((BG_PAL_REQUIRED, bgpal), (SPR_PAL_REQUIRED, sprpal)):
        have = [n for n, _ in pl]
        for n in names:
            if n not in have:
                raise AssetError('missing palette %s' % n)
    palnum = dict((bgpal[n][0], n) for n in range(8))
    if deckpal[0][1] != bgpal[palnum['PAL_WALL']][1]:
        raise AssetError('deck palette 0 must equal PAL_WALL')

    # ---- font / ui / sprites ------------------------------------------------
    missing = [chr(c) for c in range(32, 96) if chr(c) not in font]
    if missing:
        raise AssetError('font missing glyphs: %r' % ''.join(missing))
    font_tiles = [font[chr(c)] for c in range(32, 96)]
    if any(any(r) for r in font_tiles[0]):
        raise AssetError('space glyph must be blank')

    ui_names = [a for n, _ in ui if n for a in n.split('|')]
    for n in UI_REQUIRED:
        if n not in ui_names:
            raise AssetError('missing UI tile %s' % n)
    if len(ui) > NUM_UI:
        raise AssetError('too many UI tiles (%d > %d)' % (len(ui), NUM_UI))
    i = 0
    while len(ui) < NUM_UI:
        ui.append(('UI_FREE_%d' % i, BLANK))
        i += 1

    spr_names = [a for n, _ in spr if n for a in n.split('|')]
    for n in SPR_REQUIRED:
        if n not in spr_names:
            raise AssetError('missing sprite %s' % n)
    if len(spr) > MAX_SPR_ART:
        raise AssetError('too many sprite tiles (%d > %d: tiles %d..%d are boss art)'
                         % (len(spr), MAX_SPR_ART, BOSS_TILE_BASE, MAX_SPR - 1))
    assert len(spr) <= BOSS_TILE_BASE and BOSS_TILE_BASE + BOSS_TILES == MAX_SPR
    for n in BOSS_ORDER:
        if n not in boss:
            raise AssetError('missing @boss %s' % n)

    # ---- room art: walls, static metatiles, animated metatiles ------------
    for v in WALL_VARIANTS:
        if v not in walls:
            raise AssetError('missing @wall %s' % v)
    anim_mts = dict((a[0], name) for name, a in anims.items())
    for n in ANIM_ORDER:
        if n not in anims:
            raise AssetError('missing animation %s' % n)
    for n in list(metas) + list(anim_mts):
        if n not in MT_ORDER:
            raise AssetError('unknown metatile %s' % n)
    for n in MT_ORDER:
        if (n in metas) == (n in anim_mts):
            raise AssetError('metatile %s needs exactly one @meta or @anim' % n)

    art = []        # unique tiles, art[i] lives at TILE_ART_BASE + i
    seen = {tkey(BLANK): TILE_FONT_BASE}   # blank -> the space glyph (tile 0)
    owner = {}      # art index -> name, for the art labels

    def alloc(tile, label, shared=True):
        key = tkey(tile)
        if shared and key in seen:
            return seen[key]
        idx = TILE_ART_BASE + len(art)
        art.append(tile)
        owner[idx] = label
        if shared:
            seen[key] = idx
        return idx

    wall_tile = [0] * (4 * len(WALL_VARIANTS))
    for vi, v in enumerate(WALL_VARIANTS):
        for c in range(4):
            wall_tile[c * len(WALL_VARIANTS) + vi] = alloc(
                walls[v][c], 'wall %s %s' % (WALL_CORNERS[c], v))
    mt_list = []    # (name, [4 abs idx], palnum, [4 tiles])
    for n in MT_ORDER:
        if n in metas:
            tiles, pal = metas[n]
            shared = True
        else:
            mt, pal, speed, frames = anims[anim_mts[n]]
            tiles = frames[0]
            shared = False
        if pal not in palnum:
            raise AssetError('%s: unknown palette %s' % (n, pal))
        mt_list.append([n, None, palnum[pal], tiles, shared])
    # static metatiles first (deduped), then the exclusive animated ones
    for m in mt_list:
        if m[4]:
            m[1] = [alloc(t, m[0] + ' ' + 'TL TR BL BR'.split()[c])
                    for c, t in enumerate(m[3])]
    for m in mt_list:
        if not m[4]:
            m[1] = [alloc(t, m[0] + ' ' + 'TL TR BL BR'.split()[c], shared=False)
                    for c, t in enumerate(m[3])]
    mt_list = [tuple(m[:4]) for m in mt_list]
    if len(art) > MAX_ART:
        raise AssetError('room art uses %d tiles (> %d)' % (len(art), MAX_ART))
    anim_list = []
    mt_index = dict((m[0], i) for i, m in enumerate(mt_list))
    for n in ANIM_ORDER:
        mt, pal, speed, frames = anims[n]
        anim_list.append((n, mt_index[mt], speed, frames))

    # ---- logo -------------------------------------------------------------
    if logo_block is None or attr_block is None:
        raise AssetError('missing @logo / @logo_attr')
    logo_w = logo_block.w // 8
    logo_h = logo_block.h // 8
    if logo_w > 20 or logo_h > 18:
        raise AssetError('logo too big (%dx%d tiles)' % (logo_w, logo_h))
    cells = split_tiles(logo_block)
    fixed = {}  # encoded -> absolute index for font+ui tiles reusable as-is
    for i, t in enumerate(font_tiles):
        fixed.setdefault(tkey(t), TILE_FONT_BASE + i)
    for i, (_, t) in enumerate(ui):
        fixed.setdefault(tkey(t), TILE_UI_BASE + i)
    uniq = []
    lseen = {}
    logo_map = []
    for t in cells:
        e = tkey(t)
        if e in fixed:
            logo_map.append(fixed[e])
            continue
        if e not in lseen:
            lseen[e] = len(uniq)
            uniq.append(t)
        logo_map.append(TILE_ART_BASE + lseen[e])
    if strict and len(uniq) > MAX_LOGO_TILES:
        raise AssetError('logo uses %d unique tiles (> %d)' %
                         (len(uniq), MAX_LOGO_TILES))
    if attr_block.h != logo_h or attr_block.w != logo_w:
        raise AssetError('logo_attr must be %dx%d chars' % (logo_w, logo_h))
    logo_attr = []
    for row in attr_block.rows:
        for ch in row:
            if ch not in '01234567':
                raise AssetError('logo_attr: bad palette %r' % ch)
            logo_attr.append(int(ch))

    return {
        'font': font_tiles,
        'ui': ui,
        'walls': walls,
        'wall_tile': wall_tile,
        'art': art,
        'art_owner': owner,
        'mt': mt_list,
        'anim': anim_list,
        'spr': spr,
        'boss': [(n, boss[n]) for n in BOSS_ORDER],
        'logo_w': logo_w,
        'logo_h': logo_h,
        'logo_tiles': uniq,
        'logo_cells': cells,
        'logo_map': logo_map,
        'logo_attr': logo_attr,
        'bgpal': bgpal,
        'sprpal': sprpal,
        'deckpal': deckpal,
        'aliases': aliases,
    }


# --------------------------------------------------------------------------
# C output
# --------------------------------------------------------------------------

def c_bytes(data, indent='    ', per_line=16):
    lines = []
    for i in range(0, len(data), per_line):
        lines.append(indent + ','.join('0x%02X' % b for b in data[i:i + per_line]) + ',')
    return '\n'.join(lines)


def tile_array(name, count_expr, tiles, labels=None):
    s = ['const uint8_t %s[%s] = {' % (name, count_expr)]
    for i, t in enumerate(tiles):
        if labels and labels[i]:
            s.append('    /* %s */' % labels[i])
        s.append(c_bytes(encode_tile(t)))
    s.append('};')
    return '\n'.join(s)


def enum_block(items, comment=None):
    s = []
    if comment:
        s.append('/* %s */' % comment)
    s.append('enum {')
    for i, (n, v) in enumerate(items):
        sep = ',' if i < len(items) - 1 else ''
        if v is None:
            s.append('    %s%s' % (n, sep))
        else:
            s.append('    %s = %s%s' % (n, v, sep))
    s.append('};')
    return '\n'.join(s)


def font_label(i):
    ch = chr(32 + i)
    if ch == ' ':
        return "' '"
    if ch == '\\':
        return 'backslash'
    return "'%s'" % ch


def pal_rows(arr, count_expr, pals):
    s = ['const uint16_t %s[%s] = {' % (arr, count_expr)]
    for name, cols in pals:
        s.append('    ' + ','.join('0x%04X' % rgb555(c) for c in cols) +
                 ', /* %s %s */' % (name, ' '.join('#%02x%02x%02x' % c for c in cols)))
    s.append('};')
    return '\n'.join(s)


def gen(data):
    H = []
    a = H.append
    a('/* AUTO-GENERATED by tools/gen_assets.py from the assets/ directory -- DO NOT EDIT */')
    a('/* Contract: docs/CONTRACTS.md sections 2-3.  All data lives in a switchable ROM')
    a('   bank (BANKREF(assets)); map it in with SWITCH_ROM(BANK(assets)) to read it. */')
    a('#ifndef ALIGNMENT_ASSETS_H')
    a('#define ALIGNMENT_ASSETS_H')
    a('')
    a('#include <stdint.h>')
    a('')
    a('/* ---- BG tile layout (signed 0x8800 mode; sprites own 0x8000-0x87FF) ---- */')
    a('#define TILE_FONT_BASE %d' % TILE_FONT_BASE)
    a('#define TILE_UI_BASE   %d' % TILE_UI_BASE)
    a('#define TILE_ART_BASE  %d' % TILE_ART_BASE)
    a('#define TILE_BLANK     0     /* the space glyph: all colour 0 (black) */')
    a('#define FONT_TILE(ch)  ((uint8_t)((ch) - 32))      /* ASCII 32..95 */')
    a('')
    a('extern const uint8_t font_tiles[64*16];')
    a('extern const uint8_t ui_tiles[16*16];')
    a('')
    ui_items = []
    for i, (n, _) in enumerate(data['ui']):
        for al in n.split('|'):
            ui_items.append((al, 'TILE_UI_BASE + %d' % i))
    a(enum_block(ui_items, 'UI tiles: absolute BG indices.  Box edges are symmetric:\n'
                 '   UI_BOX_B == UI_BOX_T and UI_BOX_R == UI_BOX_L; box interior = TILE_BLANK.\n'
                 '   Bars: UI_BAR_FULL / UI_BAR_HALF, empty = UI_BAR_EMPTY.'))
    a('')
    a('/* ---- room art: one block of tile data loaded at TILE_ART_BASE ---- */')
    a('#define NUM_ART_TILES %d' % len(data['art']))
    a('extern const uint8_t art_tiles[NUM_ART_TILES*16];')
    a('')
    a('/* Walls are auto-tiled per 8x8 quarter: wall_tile[corner*WALL_VARIANTS + variant].')
    a('   For a quarter: "vertical neighbour" = the cell above (top corners) or below')
    a('   (bottom corners); "horizontal neighbour" = left (left corners) or right.')
    a('   FILL: both + diagonal are wall.  HEDGE: vertical neighbour open.  VEDGE:')
    a('   horizontal neighbour open.  OUTER: both open.  INNER: both wall, diagonal open.')
    a('   FILL2: alternative fill (lit panel); each quarter is self-contained, so it can')
    a('   replace FILL on any quarter.  Cells outside the room count as wall. */')
    a(enum_block([(n, None) for n in WALL_CORNERS]))
    a(enum_block([('WV_' + n, None) for n in WALL_VARIANTS] + [('WALL_VARIANTS', None)]))
    a('extern const uint8_t wall_tile[4*WALL_VARIANTS];   /* absolute BG indices */')
    a('')
    mt_items = [(n, None) for n, _, _, _ in data['mt']] + [('NUM_MT', None)]
    a(enum_block(mt_items, 'metatiles: 4 absolute BG indices each (TL,TR,BL,BR) + a CGB palette'))
    a('extern const uint8_t mt_tiles[NUM_MT*4];')
    a('extern const uint8_t mt_cgb_pal[NUM_MT];')
    a('')
    a('/* tile animations: frame k (4 tiles = 64 bytes, TL,TR,BL,BR) of animation a is')
    a('   anim_data[a] + k*64; copy it over the 4 tiles of metatile anim_mt[a] every')
    a('   anim_speed[a] video frames.  Frame 0 is the resting art; animated metatiles')
    a('   own their 4 tiles (no other metatile or wall uses them). */')
    a(enum_block([(n, None) for n in ANIM_ORDER] + [('NUM_ANIM', None)]))
    a('extern const uint8_t anim_mt[NUM_ANIM];')
    a('extern const uint8_t anim_frames[NUM_ANIM];')
    a('extern const uint8_t anim_speed[NUM_ANIM];')
    a('extern const uint8_t *const anim_data[NUM_ANIM];')
    a('')
    a('/* ---- sprites (8x8 OAM mode), tiles 0..127 at 0x8000 ---- */')
    a('#define NUM_SPR_TILES %d' % len(data['spr']))
    a('extern const uint8_t spr_tiles[NUM_SPR_TILES*16];')
    spr_items = []
    for i, (n, _) in enumerate(data['spr']):
        if n:
            for al in n.split('|'):
                spr_items.append((al, '%d' % i))
    a(enum_block(spr_items,
                 'sprite tile indices.  16x16 frames (MECH, BOT, CRATE, TURRET, EXPLODE)\n'
                 '   are 4 tiles TL,TR,BL,BR.  Mech frames (incl. MECH_SHOOT, cannon muzzle at\n'
                 '   x=15,y=7) face right with gravity down: OAM X flip for left, Y flip for\n'
                 '   gravity up.  TURRET: floor-mounted (base on row 15), barrel right: X flip\n'
                 '   = left, Y flip = ceiling.  SHOT / SHOT_POWER travel right (X flip = left);\n'
                 '   SHOT_UP / SHOT_POWER_UP travel up (Y flip = down).  MECH_SHOOT_UP aims\n'
                 '   overhead (muzzle x=13..14, y=0).\n'
                 '   SPR_RING is the TL quarter of a 16x16 ring (draw 4 with X/Y flips).\n'
                 '   The rest are single 8x8 tiles.  Tiles BOSS_TILE_BASE..127 are boss art.'))
    a('')
    a('/* ---- boss art: the game copies one boss (2 frames of 32x32 = 32 tiles) into')
    a('   sprite tiles BOSS_TILE_BASE..127.  Frame f, tile k (4x4 row-major inside the')
    a('   32x32 frame) is boss_tiles[b] + (f*16+k)*16.  Colours = SPAL_BOT / DMG_OBP1. */')
    a(enum_block([(n, None) for n in BOSS_ORDER] + [('NUM_BOSS_ART', None)]))
    a('#define BOSS_TILE_BASE %d          /* NUM_SPR_TILES <= BOSS_TILE_BASE */' % BOSS_TILE_BASE)
    a('extern const uint8_t boss_tiles[NUM_BOSS_ART][32*16];')
    a('')
    a('/* ---- title logo: tiles load at TILE_ART_BASE; map holds ABSOLUTE indices ---- */')
    a('#define LOGO_W %d' % data['logo_w'])
    a('#define LOGO_H %d' % data['logo_h'])
    a('#define LOGO_NTILES %d' % len(data['logo_tiles']))
    a('extern const uint8_t logo_tiles[LOGO_NTILES*16];')
    a('extern const uint8_t logo_map[LOGO_W*LOGO_H];')
    a('extern const uint8_t logo_cgb_attr[LOGO_W*LOGO_H]; /* CGB BG palette per logo cell */')
    a('')
    a('/* ---- CGB palettes (RGB555: r | g<<5 | b<<10); every BG colour 0 is black ---- */')
    a('/* BG palette numbers */')
    for n, (name, cols) in enumerate(data['bgpal']):
        a('#define %s %d' % (name, n))
    a('/* sprite palette numbers */')
    for n, (name, cols) in enumerate(data['sprpal']):
        a('#define %s %d' % (name, n))
    if data['aliases']:
        a('/* palette roles */')
        for n, t in data['aliases']:
            a('#define %s %s' % (n, t))
    a('extern const uint16_t bg_cgb_pal[8*4];')
    a('extern const uint16_t spr_cgb_pal[8*4];')
    a('')
    a('/* hull palettes per station deck: copy deck_cgb_pal + d*4 into BG palette PAL_WALL.')
    a('   Deck 0 is the default PAL_WALL. */')
    a('#define NUM_DECK_PALS %d' % len(data['deckpal']))
    for n, (name, cols) in enumerate(data['deckpal']):
        a('#define %s %d' % (name, n))
    a('extern const uint16_t deck_cgb_pal[NUM_DECK_PALS*4];')
    a('')
    a('/* ---- DMG palettes: inverted so colour index 0 = black, 3 = white ---- */')
    a('#define DMG_BGP  0x%02X   /* 0 black, 1 dark grey, 2 light grey, 3 white */' % DMG_BGP)
    a('#define DMG_OBP0 0x%02X   /* mech, crate, sparks, UI sprites: same ramp as BG */' % DMG_OBP0)
    a('#define DMG_OBP1 0x%02X   /* drones (+ their wrecks): 1 black outline, 2 light grey, 3 white eye */' % DMG_OBP1)
    a('')
    a('#endif')
    a('')

    C = []
    c = C.append
    c('/* AUTO-GENERATED by tools/gen_assets.py from the assets/ directory -- DO NOT EDIT */')
    c('/* On the Game Boy all art lives in a switchable ROM bank: map it in to read it. */')
    c('#ifdef __SDCC')
    c('#pragma bank 255')
    c('#include <gb/gb.h>')
    c('BANKREF(assets)')
    c('#endif')
    c('#include "assets.h"')
    c('')
    c(tile_array('font_tiles', '64*16', data['font'],
                 [font_label(i) for i in range(64)]))
    c('')
    c(tile_array('ui_tiles', '16*16', [t for _, t in data['ui']],
                 [n for n, _ in data['ui']]))
    c('')
    labels = [data['art_owner'][TILE_ART_BASE + i] + ' (%d)' % (TILE_ART_BASE + i)
              for i in range(len(data['art']))]
    c(tile_array('art_tiles', 'NUM_ART_TILES*16', data['art'], labels))
    c('')
    c('const uint8_t wall_tile[4*WALL_VARIANTS] = {')
    nv = len(WALL_VARIANTS)
    for ci in range(4):
        c('    ' + ','.join('%3d' % v for v in data['wall_tile'][ci * nv:(ci + 1) * nv]) +
          ', /* %s: %s */' % (WALL_CORNERS[ci], ' '.join(WALL_VARIANTS)))
    c('};')
    c('')
    c('const uint8_t mt_tiles[NUM_MT*4] = {')
    for n, idx, _, _ in data['mt']:
        c('    ' + ','.join('%3d' % v for v in idx) + ', /* %s */' % n)
    c('};')
    c('')
    c('const uint8_t mt_cgb_pal[NUM_MT] = {')
    for n, _, p, _ in data['mt']:
        c('    %d, /* %s */' % (p, n))
    c('};')
    c('')
    c('const uint8_t anim_mt[NUM_ANIM] = {')
    for n, mt, sp, fr in data['anim']:
        c('    %s, /* %s */' % (data['mt'][mt][0], n))
    c('};')
    c('const uint8_t anim_frames[NUM_ANIM] = {')
    c('    ' + ', '.join('%d' % len(fr) for _, _, _, fr in data['anim']) + ',')
    c('};')
    c('const uint8_t anim_speed[NUM_ANIM] = {')
    c('    ' + ', '.join('%d' % sp for _, _, sp, _ in data['anim']) + ',')
    c('};')
    c('')
    total = sum(len(fr) for _, _, _, fr in data['anim'])
    c('static const uint8_t anim_frame_data[%d*64] = {' % total)
    offs = []
    pos = 0
    for n, mt, sp, fr in data['anim']:
        offs.append(pos)
        for k, f in enumerate(fr):
            c('    /* %s frame %d */' % (n, k))
            for t in f:
                c(c_bytes(encode_tile(t)))
            pos += 64
    c('};')
    c('const uint8_t *const anim_data[NUM_ANIM] = {')
    for (n, _, _, _), o in zip(data['anim'], offs):
        c('    anim_frame_data + %d, /* %s */' % (o, n))
    c('};')
    c('')
    c(tile_array('spr_tiles', 'NUM_SPR_TILES*16', [t for _, t in data['spr']],
                 [n for n, _ in data['spr']]))
    c('')
    c('const uint8_t boss_tiles[NUM_BOSS_ART][32*16] = {')
    for n, tiles in data['boss']:
        c('  { /* %s */' % n)
        for i, t in enumerate(tiles):
            c('    /* frame %d tile %d */' % (i // 16, i % 16))
            c(c_bytes(encode_tile(t)))
        c('  },')
    c('};')
    c('')
    c(tile_array('logo_tiles', 'LOGO_NTILES*16', data['logo_tiles']))
    c('')
    lw = data['logo_w']
    c('const uint8_t logo_map[LOGO_W*LOGO_H] = {')
    for r in range(data['logo_h']):
        c('    ' + ','.join('%3d' % v for v in data['logo_map'][r * lw:(r + 1) * lw]) + ',')
    c('};')
    c('')
    c('const uint8_t logo_cgb_attr[LOGO_W*LOGO_H] = {')
    for r in range(data['logo_h']):
        c('    ' + ','.join('%d' % v for v in data['logo_attr'][r * lw:(r + 1) * lw]) + ',')
    c('};')
    c('')
    c(pal_rows('bg_cgb_pal', '8*4', data['bgpal']))
    c('')
    c(pal_rows('spr_cgb_pal', '8*4', data['sprpal']))
    c('')
    c(pal_rows('deck_cgb_pal', 'NUM_DECK_PALS*4', data['deckpal']))
    c('')
    return '\n'.join(H), '\n'.join(C)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--assets', default=os.path.join(ROOT, 'assets'))
    ap.add_argument('--out-dir', default=os.path.join(ROOT, 'src', 'gb'))
    ap.add_argument('--check', action='store_true',
                    help='do not write; exit 1 if the outputs are out of date')
    args = ap.parse_args(argv)
    try:
        data = load_assets(args.assets)
    except AssetError as e:
        sys.stderr.write('gen_assets: error: %s\n' % e)
        return 1
    h, c = gen(data)
    stale = []
    for fn, txt in (('assets.h', h), ('assets.c', c)):
        p = os.path.join(args.out_dir, fn)
        old = None
        if os.path.exists(p):
            with open(p, 'r') as f:
                old = f.read()
        if old != txt:
            stale.append(p)
            if not args.check:
                if not os.path.isdir(args.out_dir):
                    os.makedirs(args.out_dir)
                with open(p, 'w') as f:
                    f.write(txt)
    if args.check:
        if stale:
            sys.stderr.write('gen_assets: out of date: %s (run python3 tools/gen_assets.py)\n'
                             % ', '.join(stale))
            return 1
        return 0
    print('gen_assets: %d art tiles, %d sprite tiles, logo %dx%d (%d unique tiles) -> %s' %
          (len(data['art']), len(data['spr']), data['logo_w'], data['logo_h'],
           len(data['logo_tiles']), args.out_dir))
    return 0


if __name__ == '__main__':
    sys.exit(main())
