#!/usr/bin/env python3
"""Render PNG previews of every ALIGNMENT asset into build/preview/.

    python3 tools/preview_assets.py [--out-dir DIR] [--scale N]

Writes (at N x scale, default 3):
  sheet.png          every tile: font, UI, wall pieces, metatiles + animation
                     frames, sprites, logo tiles, palettes (CGB and DMG)
  room1_cgb.png / room1_dmg.png   mock gameplay screen, gravity down
  room2_cgb.png / room2_dmg.png   mock gameplay screen, gravity up, sealed exit
  title_cgb.png / title_dmg.png   title screen with the logo
  sprites.png        mech / drone frames enlarged (both mech palettes + DMG)
  seams.png          random wall blobs auto-tiled, to check the seams
  decks.png          room 1 with every deck hull palette
  anims.png          every tile animation, frame by frame
  bosses.png         boss art (both frames, CGB SPAL_BOT + DMG OBP1, flipped)
  action_cgb.png / action_dmg.png   mock action screen: turrets, shots, items,
                     decoration, boss door, explosion, HUD pips

Mirrors the game's rendering rules: 0x8800 BG tiles, per-quarter wall
autotiling (docs/CONTRACTS.md), 8x8 OAM sprites with X/Y flip.  Requires
Pillow.
"""

import argparse
import os
import random
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_assets as G  # noqa: E402

ROOT = G.ROOT
# DMG LCD shades, index = shade number (0 lightest .. 3 darkest)
DMG_SHADES = [(224, 232, 216), (150, 162, 150), (72, 84, 84), (8, 12, 16)]
SHEET_BG = (30, 26, 44)
LABEL = (190, 170, 230)

RW, RH = 10, 9
WC = dict((n, i) for i, n in enumerate(G.WALL_CORNERS))
WV = dict((n, i) for i, n in enumerate(G.WALL_VARIANTS))
MT = dict((n, i) for i, n in enumerate(G.MT_ORDER))


def c555(c):
    v = G.rgb555(c)
    r, g, b = v & 31, (v >> 5) & 31, (v >> 10) & 31
    return tuple((x << 3) | (x >> 2) for x in (r, g, b))


def cgb_pal(pal):
    return [c555(c) for c in pal[1]]


def dmg_pal(reg):
    return [DMG_SHADES[(reg >> (2 * i)) & 3] for i in range(4)]


def flip_tile(tile, fx, fy):
    if fx:
        tile = [list(reversed(r)) for r in tile]
    if fy:
        tile = list(reversed(tile))
    return tile


# --------------------------------------------------------------------------
# A Game Boy screen: 20x18 BG map + sprites
# --------------------------------------------------------------------------

class Screen(object):
    def __init__(self, data, deck=0):
        self.d = data
        self.bg = {}
        for i, t in enumerate(data['font']):
            self.bg[G.TILE_FONT_BASE + i] = t
        for i, (_, t) in enumerate(data['ui']):
            self.bg[G.TILE_UI_BASE + i] = t
        for i, t in enumerate(data['art']):
            self.bg[G.TILE_ART_BASE + i] = t
        self.spr = [t for _, t in data['spr']]
        self.map = [[0] * 20 for _ in range(18)]
        self.attr = [[0] * 20 for _ in range(18)]
        self.sprites = []  # (x, y, tile, cgbpal, dmgpal(0/1), fx, fy)
        self.bgpal = [cgb_pal(p) for p in data['bgpal']]
        wall = [n for n, _ in data['bgpal']].index('PAL_WALL')
        self.bgpal[wall] = cgb_pal(data['deckpal'][deck])
        self.objpal = [cgb_pal(p) for p in data['sprpal']]

    def load_logo(self):
        for i, t in enumerate(self.d['logo_tiles']):
            self.bg[G.TILE_ART_BASE + i] = t

    def anim_frame(self, a, k):
        """Copy frame k of animation a over its metatile's tiles."""
        name, mt, speed, frames = self.d['anim'][a]
        for c in range(4):
            self.bg[self.d['mt'][mt][1][c]] = frames[k % len(frames)][c]

    def put(self, x, y, tile, pal=0):
        if 0 <= x < 20 and 0 <= y < 18:
            self.map[y][x] = tile
            self.attr[y][x] = pal

    def text(self, x, y, s, pal=0):
        for i, ch in enumerate(s):
            self.put(x + i, y, ord(ch) - 32, pal)

    def meta(self, cx, cy, m):
        _, idx, pal, _ = self.d['mt'][m]
        for c in range(4):
            self.put(cx * 2 + (c & 1), cy * 2 + (c >> 1), idx[c], pal)

    def spr8(self, x, y, tile, pal, dmg=0, fx=False, fy=False):
        self.sprites.append((x, y, tile, pal, dmg, fx, fy))

    def spr16(self, x, y, base, pal, dmg=0, fx=False, fy=False):
        for c in range(4):
            sx, sy = c & 1, c >> 1
            if fx:
                sx = 1 - sx
            if fy:
                sy = 1 - sy
            self.sprites.append((x + sx * 8, y + sy * 8, base + c, pal, dmg, fx, fy))

    def render(self, cgb):
        img = Image.new('RGB', (160, 144))
        px = img.load()
        bgp = G.DMG_BGP
        for ty in range(18):
            for tx in range(20):
                pal = self.bgpal[self.attr[ty][tx]] if cgb else dmg_pal(bgp)
                t = self.bg[self.map[ty][tx]]
                for yy in range(8):
                    for xx in range(8):
                        px[tx * 8 + xx, ty * 8 + yy] = pal[t[yy][xx]]
        for (x, y, t, sp, dp, fx, fy) in reversed(self.sprites):
            tile = flip_tile(self.spr[t], fx, fy)
            pal = self.objpal[sp] if cgb else dmg_pal(G.DMG_OBP1 if dp else G.DMG_OBP0)
            for yy in range(8):
                for xx in range(8):
                    v = tile[yy][xx]
                    if v and 0 <= x + xx < 160 and 0 <= y + yy < 144:
                        px[x + xx, y + yy] = pal[v]
        return img


# --------------------------------------------------------------------------
# Rooms
# --------------------------------------------------------------------------

def wall_variant(is_wall, cx, cy, corner, fill2):
    dx = 1 if corner & 1 else -1
    dy = 1 if corner & 2 else -1
    v = is_wall(cx, cy + dy)
    h = is_wall(cx + dx, cy)
    d = is_wall(cx + dx, cy + dy)
    if not v and not h:
        return WV['OUTER']
    if not v:
        return WV['HEDGE']
    if not h:
        return WV['VEDGE']
    if not d:
        return WV['INNER']
    return WV['FILL2'] if fill2 else WV['FILL']


def draw_walls(s, grid):
    h = len(grid)
    w = len(grid[0])

    def is_wall(x, y):
        if x < 0 or y < 0 or x >= w or y >= h:
            return True
        return grid[y][x] == '#'

    for cy in range(h):
        for cx in range(w):
            if grid[cy][cx] != '#':
                continue
            f2 = (cx * 7 + cy * 13 + cx * cy) % 6 == 0
            for c in range(4):
                v = wall_variant(is_wall, cx, cy, c, f2)
                tile = s.d['wall_tile'][c * len(G.WALL_VARIANTS) + v]
                s.put(cx * 2 + (c & 1), cy * 2 + (c >> 1), tile,
                      [n for n, _ in s.d['bgpal']].index('PAL_WALL'))


def spr_index(data):
    return dict((a, i) for i, (n, _) in enumerate(data['spr']) if n for a in n.split('|'))


ROOM1 = [
    '##########',
    '#2...1...#',
    '#.c..#####',
    '#..###..cE',
    '#...3#...#',
    '###..pp.##',
    '#3....gg.#',
    '#xx#.f#d.#',
    '##########',
]

ROOM2 = [
    '##########',
    '#..#...F.#',
    '#x....g#o#',
    '#xx##.gg.#',
    '#.p..1.###',
    '#.3.#....#',
    '##.....c.#',
    '#..2..#..#',
    '##########',
]

ROOM3 = [
    '##########',
    '#--l--w.B#',
    '#|......B#',
    '#|......B#',
    '#|######B#',
    '#|.P.S..B#',
    '#|w..r####',
    '#+--X--t.#',
    '##########',
]


def room_screen(data, grid, gravity_up, deck=0, exit_open=True):
    s = Screen(data, deck)
    draw_walls(s, grid)
    for cy, row in enumerate(grid):
        for cx, ch in enumerate(row):
            m = None
            if ch == '1':
                m = 'MT_BACK1'
            elif ch == '2':
                m = 'MT_BACK2'
            elif ch == '3':
                m = 'MT_BACK3'
            elif ch == '.':
                m = 'MT_EMPTY'
            elif ch == 'x':
                m = 'MT_PLASMA'
            elif ch == 'c':
                m = 'MT_CELL'
            elif ch == 'k':
                m = 'MT_CRATE'
            elif ch == 'p':
                m = 'MT_PHASE_P_OFF' if gravity_up else 'MT_PHASE_P_ON'
            elif ch == 'g':
                m = 'MT_PHASE_G_ON' if gravity_up else 'MT_PHASE_G_OFF'
            elif ch == 'f':
                m = 'MT_PLATE_F'
            elif ch == 'F':
                m = 'MT_PLATE_F_ON'
            elif ch == 'd':
                m = 'MT_DOOR_CLOSED'
            elif ch == 'o':
                m = 'MT_DOOR_OPEN'
            elif ch == 'E':
                m = 'MT_EXIT_R' if exit_open else 'MT_BOSS_DOOR'
            elif ch == 'B':
                m = 'MT_BOSS_DOOR'
            elif ch in '-|+wlgt':
                m = {'-': 'MT_DECO_PIPE', '|': 'MT_DECO_PIPE_V', '+': 'MT_DECO_GRATE',
                     'w': 'MT_DECO_WINDOW', 'l': 'MT_DECO_LIGHT', 'g': 'MT_DECO_GRATE',
                     't': 'MT_DECO_LIGHT'}[ch]
            elif ch in 'PSXr':
                m = {'P': 'MT_ITEM_POWER', 'S': 'MT_ITEM_SHIELD', 'X': 'MT_ITEM_BOMB',
                     'r': 'MT_ITEM_REPAIR'}[ch]
            if m:
                s.meta(cx, cy, MT[m])
    return s


def mockup1(data, deck=0):
    s = room_screen(data, ROOM1, False, deck)
    sp = spr_index(data)
    pal = dict((n, i) for i, (n, _) in enumerate(data['sprpal']))
    # the mech, just through the entry hatch, walking right
    s.spr16(1 * 16 + 6, 4 * 16, sp['SPR_MECH_WALK1'], pal['SPAL_MECH_P'])
    # drones
    s.spr16(4 * 16, 6 * 16, sp['SPR_BOT_A'], pal['SPAL_BOT'], 1, fx=True)
    s.spr16(7 * 16, 3 * 16, sp['SPR_BOT_WALK'], pal['SPAL_BOT'], 1)
    # a crate resting on the ledge, one falling
    s.spr16(4 * 16, 4 * 16, sp['SPR_CRATE'], pal['SPAL_CRATE'])
    s.spr16(8 * 16, 5 * 16 - 6, sp['SPR_CRATE'], pal['SPAL_CRATE'])
    # gravity rain + sparks + hint arrow
    rnd = random.Random(7)
    for i in range(7):
        x = rnd.randrange(16, 140)
        y = rnd.randrange(16, 120)
        s.spr8(x, y, sp['SPR_STREAK'], pal['SPAL_SPARK_P'])
    s.spr8(2 * 16 + 4, 2 * 16 - 4, sp['SPR_SPARK_SMALL'], pal['SPAL_SPARK_G'])
    s.spr8(3 * 16 + 6, 4 * 16 + 3, sp['SPR_SHOT'], pal['SPAL_SPARK_P'])
    return s


def mockup2(data, deck=1):
    s = room_screen(data, ROOM2, True, deck, exit_open=False)
    sp = spr_index(data)
    pal = dict((n, i) for i, (n, _) in enumerate(data['sprpal']))
    # gravity up: everything rests against ceilings (Y flip); the mech faces left
    s.spr16(4 * 16, 6 * 16, sp['SPR_MECH_IDLE'], pal['SPAL_MECH_G'], fx=True, fy=True)
    s.spr16(3 * 16, 4 * 16, sp['SPR_BOT_B'], pal['SPAL_BOT'], 1, fy=True)
    s.spr16(6 * 16, 1 * 16, sp['SPR_BOT_WALK'], pal['SPAL_BOT'], 1, fx=True, fy=True)
    s.spr16(2 * 16, 1 * 16, sp['SPR_CRATE'], pal['SPAL_CRATE'])
    rnd = random.Random(3)
    for i in range(7):
        x = rnd.randrange(16, 140)
        y = rnd.randrange(16, 120)
        s.spr8(x, y, sp['SPR_STREAK'], pal['SPAL_SPARK_G'], fy=True)
    s.spr8(5 * 16 + 4, 3 * 16 + 4, sp['SPR_SPARK_BIG'], pal['SPAL_SPARK_G'])
    return s


def action_screen(data):
    """Metal Storm style: the mech shooting turrets, items, decor, boss door."""
    s = room_screen(data, ROOM3, False)
    sp = spr_index(data)
    pal = dict((n, i) for i, (n, _) in enumerate(data['sprpal']))
    bg = dict((n, i) for i, (n, _) in enumerate(data['bgpal']))
    ui = dict((a, G.TILE_UI_BASE + i) for i, (n, _) in enumerate(data['ui']) for a in n.split('|'))
    # HUD row: hull pips + shield
    for i in range(6):
        s.put(1 + i, 0, ui['UI_HULL' if i < 4 else 'UI_HULL_EMPTY'], bg['PAL_UI'])
    s.put(8, 0, ui['UI_SHIELD'], bg['PAL_GREEN'])
    s.text(12, 0, 'P 03', bg['PAL_UI'])
    # mech on the platform, firing right at a floor turret
    s.spr16(2 * 16, 3 * 16, sp['SPR_MECH_SHOOT'], pal['SPAL_MECH_P'])
    s.spr8(3 * 16 + 4, 3 * 16 + 4, sp['SPR_SHOT'], pal['SPAL_SPARK_P'])
    s.spr8(4 * 16 + 8, 3 * 16 + 4, sp['SPR_SHOT_POWER'], pal['SPAL_SPARK_G'])
    s.spr16(7 * 16, 3 * 16, sp['SPR_TURRET_FIRE'], pal['SPAL_BOT'], 1, fx=True)
    s.spr8(6 * 16, 3 * 16 + 4, sp['SPR_BOLT'], pal['SPAL_DEAD'])
    # ceiling turret (Y flip) firing down-left
    s.spr16(4 * 16, 1 * 16, sp['SPR_TURRET'], pal['SPAL_BOT'], 1, fx=True, fy=True)
    s.spr8(3 * 16 + 2, 2 * 16 + 2, sp['SPR_BOLT'], pal['SPAL_DEAD'])
    # explosions + a drone below
    s.spr16(6 * 16, 1 * 16 + 6, sp['SPR_EXPLODE1'], pal['SPAL_DEAD'])
    s.spr16(6 * 16, 5 * 16, sp['SPR_EXPLODE2'], pal['SPAL_DEAD'])
    s.spr16(2 * 16, 6 * 16, sp['SPR_BOT_WALK'], pal['SPAL_BOT'], 1)
    return s


def title_screen(data):
    s = Screen(data)
    s.load_logo()
    lw, lh = data['logo_w'], data['logo_h']
    ox, oy = (20 - lw) // 2, 2
    for y in range(lh):
        for x in range(lw):
            s.put(ox + x, oy + y, data['logo_map'][y * lw + x], data['logo_attr'][y * lw + x])
    ui = dict((a, G.TILE_UI_BASE + i) for i, (n, _) in enumerate(data['ui']) for a in n.split('|'))
    pal = dict((n, i) for i, (n, _) in enumerate(data['bgpal']))
    s.text(2, oy + lh + 1, 'GRAVITY MECH RESCUE'[:16], pal['PAL_UI'])
    s.text(1, oy + lh + 1, ' ' * 18)
    sub = 'DERELICT STATION'
    s.text((20 - len(sub)) // 2, oy + lh + 1, sub, pal['PAL_UI'])
    x0, y0, x1, y1 = 3, oy + lh + 3, 16, oy + lh + 7
    for x in range(x0 + 1, x1):
        s.put(x, y0, ui['UI_BOX_T'])
        s.put(x, y1, ui['UI_BOX_B'])
    for y in range(y0 + 1, y1):
        s.put(x0, y, ui['UI_BOX_L'])
        s.put(x1, y, ui['UI_BOX_R'])
    s.put(x0, y0, ui['UI_BOX_TL'])
    s.put(x1, y0, ui['UI_BOX_TR'])
    s.put(x0, y1, ui['UI_BOX_BL'])
    s.put(x1, y1, ui['UI_BOX_BR'])
    s.text(6, y0 + 1, 'START', pal['PAL_UI'])
    s.text(6, y0 + 2, 'ZEN MODE', pal['PAL_UI'])
    s.text(6, y0 + 3, 'OPTIONS', pal['PAL_UI'])
    s.put(13, y0 + 1, ui['UI_GRAV_DOWN'], pal['PAL_PURPLE'])
    s.put(14, y0 + 1, ui['UI_GRAV_UP'], pal['PAL_GREEN'])
    s.put(1, 17, ui['UI_CELL'], pal['PAL_GLOW'])
    s.text(2, 17, '03', pal['PAL_UI'])
    s.put(5, 17, ui['UI_SHIELD'], pal['PAL_UI'])
    s.text(6, 17, '12', pal['PAL_UI'])
    s.text(12, 17, 'V1.0', pal['PAL_UI'])
    sp = spr_index(data)
    spal = dict((n, i) for i, (n, _) in enumerate(data['sprpal']))
    s.spr8(4 * 8, (y0 + 1) * 8, sp['SPR_CURSOR'], spal['SPAL_WHITE'])
    s.spr16(140, 112, sp['SPR_MECH_IDLE'], spal['SPAL_MECH_P'], fx=True)
    s.spr16(4, 112, sp['SPR_BOT_A'], spal['SPAL_BOT'], 1)
    return s


# --------------------------------------------------------------------------
# Sheets
# --------------------------------------------------------------------------

def tile_img(tiles, pal, per_row, gap=1, transparent=False):
    n = len(tiles)
    rows = max(1, (n + per_row - 1) // per_row)
    img = Image.new('RGB', (per_row * (8 + gap), rows * (8 + gap)), SHEET_BG)
    px = img.load()
    for i, t in enumerate(tiles):
        x0, y0 = (i % per_row) * (8 + gap), (i // per_row) * (8 + gap)
        p = pal[i] if isinstance(pal[0], list) else pal
        for y in range(8):
            for x in range(8):
                px[x0 + x, y0 + y] = p[t[y][x]] if (t[y][x] or not transparent) else (0, 0, 0)
    return img


def meta_img(tiles, pal):
    img = Image.new('RGB', (16, 16))
    px = img.load()
    for c in range(4):
        for y in range(8):
            for x in range(8):
                px[(c & 1) * 8 + x, (c >> 1) * 8 + y] = pal[tiles[c][y][x]]
    return img


def stack(items, scale, pad=8, label_h=12, width=None):
    """items: list of (label, image) -> one tall image, each image scaled."""
    W = width or max(im.size[0] * scale for _, im in items) + 2 * pad
    H = sum(label_h + im.size[1] * scale + pad for _, im in items) + pad
    out = Image.new('RGB', (W, H), SHEET_BG)
    dr = ImageDraw.Draw(out)
    y = pad
    for name, im in items:
        dr.text((pad, y), name, fill=LABEL)
        out.paste(im.resize((im.size[0] * scale, im.size[1] * scale), Image.NEAREST), (pad, y + label_h))
        y += label_h + im.size[1] * scale + pad
    return out


def row_img(images, gap=2, bg=SHEET_BG):
    W = sum(i.size[0] for i in images) + gap * (len(images) - 1)
    H = max(i.size[1] for i in images)
    out = Image.new('RGB', (W, H), bg)
    x = 0
    for i in images:
        out.paste(i, (x, 0))
        x += i.size[0] + gap
    return out


def col_img(images, gap=2, bg=SHEET_BG):
    W = max(i.size[0] for i in images)
    H = sum(i.size[1] for i in images) + gap * (len(images) - 1)
    out = Image.new('RGB', (W, H), bg)
    y = 0
    for i in images:
        out.paste(i, (0, y))
        y += i.size[1] + gap
    return out


def spr16_img(data, base, pal, fx=False, fy=False):
    img = Image.new('RGB', (16, 16))
    px = img.load()
    for c in range(4):
        t = flip_tile(data['spr'][base + c][1], fx, fy)
        sx, sy = c & 1, c >> 1
        if fx:
            sx = 1 - sx
        if fy:
            sy = 1 - sy
        for y in range(8):
            for x in range(8):
                px[sx * 8 + x, sy * 8 + y] = pal[t[y][x]] if t[y][x] else (0, 0, 0)
    return img


def ring_img(data, i, pal):
    img = Image.new('RGB', (16, 16))
    px = img.load()
    for c in range(4):
        t = flip_tile(data['spr'][i][1], c & 1, c >> 1)
        for y in range(8):
            for x in range(8):
                px[(c & 1) * 8 + x, (c >> 1) * 8 + y] = pal[t[y][x]] if t[y][x] else (0, 0, 0)
    return img


def sheet(data, scale):
    bg = [cgb_pal(p) for p in data['bgpal']]
    sp = [cgb_pal(p) for p in data['sprpal']]
    bgn = dict((n, i) for i, (n, _) in enumerate(data['bgpal']))
    spn = dict((n, i) for i, (n, _) in enumerate(data['sprpal']))
    dmg = dmg_pal(G.DMG_BGP)
    items = []
    items.append(('FONT  PAL_UI / DMG', col_img([tile_img(data['font'], bg[bgn['PAL_UI']], 32),
                                                tile_img(data['font'], dmg, 32)])))
    ui = [t for _, t in data['ui']]
    items.append(('UI 64..79: ' + ' '.join(n[3:] for n, _ in data['ui']),
                  row_img([tile_img(ui, bg[bgn['PAL_UI']], 16), tile_img(ui, dmg, 16)], 6)))
    # wall pieces, each variant as a 16x16 block, CGB + DMG
    ims = []
    for v in G.WALL_VARIANTS:
        ims.append(meta_img(data['walls'][v], bg[bgn['PAL_WALL']]))
    wrow = row_img(ims, 3)
    ims = [meta_img(data['walls'][v], dmg) for v in G.WALL_VARIANTS]
    items.append(('WALLS ' + ' '.join(G.WALL_VARIANTS), col_img([wrow, row_img(ims, 3)])))
    # metatiles
    cg, dm = [], []
    for name, idx, p, tiles in data['mt']:
        cg.append(meta_img(tiles, bg[p]))
        dm.append(meta_img(tiles, dmg))
    items.append(('METATILES (CGB / DMG): ' + ' '.join(n[3:] for n, _, _, _ in data['mt'][:13]),
                  col_img([row_img(cg, 3), row_img(dm, 3)])))
    items.append(('  ... ' + ' '.join(n[3:] for n, _, _, _ in data['mt'][13:]), Image.new('RGB', (1, 1), SHEET_BG)))
    # sprites
    names = [n for n, _ in data['spr']]
    big = []
    for n in ['SPR_MECH_' + k for k in ('IDLE', 'IDLE2', 'WALK1', 'WALK2', 'FALL', 'FLIP', 'LAND', 'DEAD')]:
        i = names.index(n)
        p = sp[spn['SPAL_DEAD']] if n.endswith('DEAD') else sp[spn['SPAL_MECH_P']]
        big.append(spr16_img(data, i, p))
    big.append(spr16_img(data, names.index('SPR_MECH_SHOOT'), sp[spn['SPAL_MECH_P']]))
    for n in ['SPR_BOT_A', 'SPR_BOT_B', 'SPR_BOT_WALK', 'SPR_BOT_DEAD', 'SPR_CRATE',
              'SPR_TURRET', 'SPR_TURRET_FIRE', 'SPR_EXPLODE1', 'SPR_EXPLODE2']:
        i = names.index(n)
        p = sp[spn['SPAL_DEAD' if (n.endswith('DEAD') or 'EXPLODE' in n) else
                   ('SPAL_CRATE' if 'CRATE' in n else 'SPAL_BOT')]]
        big.append(spr16_img(data, i, p))
    small = []
    for i, (n, t) in enumerate(data['spr']):
        if n and not any(n.startswith(k) for k in G.SPR_16X16_PREFIXES + ('SPR_RING',)):
            small.append(t)
    items.append(('SPRITES 16x16', row_img(big, 3)))
    items.append(('SPRITES 8x8 + ring', row_img([tile_img(small, sp[spn['SPAL_WHITE']], 16, transparent=True),
                                                  ring_img(data, names.index('SPR_RING'), sp[spn['SPAL_SPARK_G']])], 6)))
    # palettes
    pimg = Image.new('RGB', (8 * 4 * 6 + 20, 3 * 8), SHEET_BG)
    dr = ImageDraw.Draw(pimg)
    for row, pals in enumerate((data['bgpal'], data['sprpal'], data['deckpal'])):
        for i, (n, cols) in enumerate(pals):
            for k, c in enumerate(cols):
                x = i * 26 + k * 6
                dr.rectangle([x, row * 8, x + 5, row * 8 + 6], fill=c555(c))
    items.append(('PALETTES: BG 0-7 / SPRITE 0-7 / DECK ' + ' '.join(n for n, _ in data['bgpal']), pimg))
    items.append(('LOGO TILES (%d)' % len(data['logo_tiles']),
                  tile_img(data['logo_tiles'], bg[bgn['PAL_PURPLE']], 30)))
    return stack(items, scale)


def anims_sheet(data, scale):
    bg = [cgb_pal(p) for p in data['bgpal']]
    dmg = dmg_pal(G.DMG_BGP)
    items = []
    for name, mt, speed, frames in data['anim']:
        p = bg[data['mt'][mt][2]]
        # 2x2 cells of the same metatile per frame to show tiling, CGB then DMG
        cg = []
        dm = []
        for f in frames:
            for pal, dest in ((p, cg), (dmg, dm)):
                one = meta_img(f, pal)
                four = Image.new('RGB', (32, 32))
                for k in range(4):
                    four.paste(one, ((k & 1) * 16, (k >> 1) * 16))
                dest.append(four)
        items.append(('%s (%d frames, every %d)' % (name, len(frames), speed),
                      row_img(cg + dm, 3)))
    return stack(items, scale)


def sprites_sheet(data, scale):
    sp = [cgb_pal(p) for p in data['sprpal']]
    spn = dict((n, i) for i, (n, _) in enumerate(data['sprpal']))
    names = [n for n, _ in data['spr']]
    mech = ['SPR_MECH_' + k for k in ('IDLE', 'IDLE2', 'WALK1', 'WALK2', 'FALL', 'FLIP', 'LAND', 'DEAD',
                                      'SHOOT')]
    bot = ['SPR_BOT_A', 'SPR_BOT_B', 'SPR_BOT_WALK', 'SPR_BOT_DEAD', 'SPR_CRATE', 'SPR_TURRET',
           'SPR_TURRET_FIRE', 'SPR_EXPLODE1', 'SPR_EXPLODE2']
    rows = []
    for palname, dmgreg in (('SPAL_MECH_P', None), ('SPAL_MECH_G', None), (None, G.DMG_OBP0)):
        ims = []
        for n in mech:
            if palname:
                p = sp[spn['SPAL_DEAD']] if n.endswith('DEAD') else sp[spn[palname]]
            else:
                p = dmg_pal(dmgreg)
            ims.append(spr16_img(data, names.index(n), p))
        rows.append(row_img(ims, 4, (0, 0, 0)))
    for dmgreg in (None, G.DMG_OBP1, G.DMG_OBP0):
        ims = []
        for n in bot:
            if dmgreg is None:
                p = sp[spn['SPAL_DEAD' if (n.endswith('DEAD') or 'EXPLODE' in n) else
                           ('SPAL_CRATE' if 'CRATE' in n else 'SPAL_BOT')]]
            else:
                p = dmg_pal(G.DMG_OBP0 if 'CRATE' in n else dmgreg)
            ims.append(spr16_img(data, names.index(n), p))
        rows.append(row_img(ims, 4, (0, 0, 0)))
    # flipped variants: left, gravity up
    i = names.index('SPR_MECH_IDLE')
    rows.append(row_img([spr16_img(data, i, sp[spn['SPAL_MECH_P']]),
                         spr16_img(data, i, sp[spn['SPAL_MECH_P']], fx=True),
                         spr16_img(data, i, sp[spn['SPAL_MECH_G']], fy=True),
                         spr16_img(data, i, sp[spn['SPAL_MECH_G']], fx=True, fy=True)], 4, (0, 0, 0)))
    return stack([('MECH (purple / green / DMG OBP0), BOTS (CGB / DMG OBP1 / OBP0), FLIPS', col_img(rows, 4))], scale * 2)


def boss_img(tiles, frame, pal, fx=False):
    img = Image.new('RGB', (32, 32))
    px = img.load()
    for k in range(16):
        t = flip_tile(tiles[frame * 16 + k], fx, False)
        tx, ty = k % 4, k // 4
        if fx:
            tx = 3 - tx
        for y in range(8):
            for x in range(8):
                px[tx * 8 + x, ty * 8 + y] = pal[t[y][x]] if t[y][x] else (0, 0, 0)
    return img


def bosses_sheet(data, scale):
    sp = [cgb_pal(p) for p in data['sprpal']]
    spn = dict((n, i) for i, (n, _) in enumerate(data['sprpal']))
    cg = sp[spn['SPAL_BOT']]
    dm = dmg_pal(G.DMG_OBP1)
    items = []
    for name, tiles in data['boss']:
        ims = [boss_img(tiles, f, cg) for f in (0, 1)]
        ims += [boss_img(tiles, f, dm) for f in (0, 1)]
        ims += [boss_img(tiles, 0, cg, fx=True)]
        items.append(('%s: frame 0 / 1 (CGB), DMG OBP1, X-flipped' % name, row_img(ims, 4, (0, 0, 0))))
    return stack(items, scale)


def seams_img(data, scale, seed=1):
    rnd = random.Random(seed)
    w, h = 20, 18
    # random blobs on a 20x18 cell grid, rendered as 2x2 screens (320x288)
    grid = [['.'] * w for _ in range(h)]
    for _ in range(26):
        bx, by = rnd.randrange(w), rnd.randrange(h)
        bw, bh = rnd.randrange(1, 5), rnd.randrange(1, 4)
        for y in range(by, min(h, by + bh)):
            for x in range(bx, min(w, bx + bw)):
                grid[y][x] = '#'
    for _ in range(40):
        grid[rnd.randrange(h)][rnd.randrange(w)] = '#'
    grid = [''.join(r) for r in grid]
    out = Image.new('RGB', (w * 16, h * 16))
    for qy in range(2):
        for qx in range(2):
            sub = [r[qx * 10:qx * 10 + 10] for r in grid[qy * 9:qy * 9 + 9]]
            s = Screen(data)

            def is_wall(x, y, qx=qx, qy=qy):
                gx, gy = x + qx * 10, y + qy * 9
                if gx < 0 or gy < 0 or gx >= w or gy >= h:
                    return True
                return grid[gy][gx] == '#'
            for cy in range(9):
                for cx in range(10):
                    if sub[cy][cx] != '#':
                        continue
                    f2 = (cx * 7 + cy * 13 + cx * cy) % 6 == 0
                    for c in range(4):
                        v = wall_variant(is_wall, cx, cy, c, f2)
                        s.put(cx * 2 + (c & 1), cy * 2 + (c >> 1),
                              data['wall_tile'][c * len(G.WALL_VARIANTS) + v], 1)
            out.paste(s.render(True), (qx * 160, qy * 144))
    return out.resize((out.size[0] * scale, out.size[1] * scale), Image.NEAREST)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--out-dir', default=os.path.join(ROOT, 'build', 'preview'))
    ap.add_argument('--scale', type=int, default=3)
    ap.add_argument('--docs', action='store_true',
                    help='also copy the room 1 mockups to docs/screens/')
    args = ap.parse_args(argv)
    data = G.load_assets(os.path.join(ROOT, 'assets'), strict=False)
    S = args.scale
    od = args.out_dir
    if not os.path.isdir(od):
        os.makedirs(od)

    def save(name, img, scale=S):
        if scale != 1:
            img = img.resize((img.size[0] * scale, img.size[1] * scale), Image.NEAREST)
        img.save(os.path.join(od, name))
        return img

    shots = {}
    for name, scr in (('room1', mockup1(data)), ('room2', mockup2(data)),
                      ('action', action_screen(data)), ('title', title_screen(data))):
        if name == 'room2':
            scr.anim_frame(G.ANIM_ORDER.index('ANIM_PLASMA'), 1)
        for cgb in (True, False):
            fn = '%s_%s.png' % (name, 'cgb' if cgb else 'dmg')
            shots[fn] = save(fn, scr.render(cgb))
    save('sheet.png', sheet(data, S), 1)
    save('anims.png', anims_sheet(data, S), 1)
    save('sprites.png', sprites_sheet(data, S), 1)
    save('bosses.png', bosses_sheet(data, S * 2), 1)
    save('seams.png', seams_img(data, 2), 1)
    decks = [mockup1(data, d).render(True) for d in range(len(data['deckpal']))]
    decks += [mockup2(data, d).render(True) for d in range(len(data['deckpal']))]
    grid = Image.new('RGB', (len(data['deckpal']) * 164, 2 * 148), SHEET_BG)
    for i, im in enumerate(decks):
        grid.paste(im, ((i % len(data['deckpal'])) * 164, (i // len(data['deckpal'])) * 148))
    save('decks.png', grid, 2)
    if args.docs:
        dd = os.path.join(ROOT, 'docs', 'screens')
        if not os.path.isdir(dd):
            os.makedirs(dd)
        shots['room1_cgb.png'].save(os.path.join(dd, 'mockup_cgb.png'))
        shots['room1_dmg.png'].save(os.path.join(dd, 'mockup_dmg.png'))
    print('previews -> %s (art %d/176, sprites %d/%d, logo %d/176)' %
          (od, len(data['art']), len(data['spr']), G.BOSS_TILE_BASE, len(data['logo_tiles'])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
