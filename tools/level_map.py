#!/usr/bin/env python3
"""Render a whole stage to one PNG, from the game itself.

Boots the ROM in PyBoy (headless, Game Boy Color), starts the stage through the game's
test hook, then draws the stage's prebuilt tile map (cartridge RAM banks 2 and 3: the
game builds the whole map there when a stage starts) with the tile art from video RAM
and the asset palettes, and puts the enemies, the start, the checkpoint and the boss
on top. What you see is what the game will stream onto the screen.

    python3 tools/level_map.py s 3              # sector 3   -> build/maps/sector3.png
    python3 tools/level_map.py 0x1234 4         # endless stage 4 of seed 0x1234
    python3 tools/level_map.py s 3 --scale 1 --out my.png
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tests'))
import gen_assets as G  # noqa: E402
from romlib import Game  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402

SH = 16
STAGE_W, STAGE_ARENA, STAGE_NEN, STAGE_ENEMY = 2048, 2256, 2055, 2056


def c555(v):
    r, g, b = v & 31, (v >> 5) & 31, (v >> 10) & 31
    return tuple((x << 3) | (x >> 2) for x in (r, g, b))


def tile_pixels(mem, addr):
    rows = []
    for y in range(8):
        lo, hi = mem[addr + y * 2], mem[addr + y * 2 + 1]
        rows.append([((lo >> (7 - x)) & 1) | (((hi >> (7 - x)) & 1) << 1) for x in range(8)])
    return rows


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--scale', type=int, default=2)
    ap.add_argument('--out')
    a = ap.parse_args(argv)
    data = G.load_assets(os.path.join(ROOT, 'assets'))
    g = Game(True)
    g.run(150)
    ok = g.start_sector(int(a.b)) if a.a == 's' else g.start_endless(int(a.a, 0), int(a.b))
    if not ok:
        print('level_map: the stage never started', file=sys.stderr)
        return 1
    g.run(4)
    mem = g.pb.memory
    w = g.u8('stage', STAGE_W)
    # palettes: the asset BG palettes, the deck's hull palette in PAL_WALL
    pals = [list(c) for _, c in data['bgpal']]
    deck = g.u8('stage', STAGE_W + 201)
    decks = data['deckpal']
    if decks:
        pals[1] = list(decks[deck % len(decks)][1])
    # tiles from VRAM (bank 0), LCDC bit 4 clear: i < 128 at 0x9000, else 0x8800
    cache = {}

    def bg_tile(i):
        if i not in cache:
            cache[i] = tile_pixels(mem, (0x9000 + i * 16) if i < 128 else (0x8800 + (i - 128) * 16))
        return cache[i]

    img = Image.new('RGB', (w * 16, SH * 16))
    px = img.load()
    for cx in range(w):
        for half in range(2):
            for ty in range(32):
                t = mem[2, 0xA000 + cx * 64 + half * 32 + ty]
                at = mem[3, 0xA000 + cx * 64 + half * 32 + ty]
                pal = pals[at & 7]
                tp = bg_tile(t)
                fx, fy = at & 0x20, at & 0x40
                for y in range(8):
                    for x in range(8):
                        c = tp[7 - y if fy else y][7 - x if fx else x]
                        px[cx * 16 + half * 8 + x, ty * 8 + y] = pal[c]
    # sprites: the enemies where they start, drawn from sprite VRAM
    spal = [list(c) for _, c in data['sprpal']]
    bot = spal[2]
    names = dict((n, i) for i, (n, _) in enumerate(data['spr']) if n)

    def spr16(x0, y0, base, fx=False, fy=False, pal=bot):
        for q in range(4):
            tp = tile_pixels(mem, 0x8000 + (base + q) * 16)
            qx, qy = q & 1, q >> 1
            if fx:
                qx ^= 1
            if fy:
                qy ^= 1
            for y in range(8):
                for x in range(8):
                    c = tp[7 - y if fy else y][7 - x if fx else x]
                    if c:
                        X, Y = x0 + qx * 8 + x, y0 + qy * 8 + y
                        if 0 <= X < img.width and 0 <= Y < img.height:
                            px[X, Y] = pal[c]

    d = ImageDraw.Draw(img)
    n = g.u8('stage', STAGE_NEN)
    for i in range(n):
        e = [g.u8('stage', STAGE_ENEMY + i * 8 + k) for k in range(8)]
        kind, x0, x1, y0, y1, speed = e[:6]
        if kind == 0:
            spr16(x0 * 16, y0 * 16, names.get('SPR_BOT_WALK', 0), fy=bool(y1))
            d.line((x0 * 16 + 2, y0 * 16 + (2 if y1 else 14), x1 * 16 + 14, y0 * 16 + (2 if y1 else 14)), fill=(255, 44, 168))
        elif kind == 1:
            spr16(x0 * 16, y0 * 16, names.get('SPR_BOT_A', 0))
            d.line((x0 * 16 + 8, y0 * 16 + 8, x0 * 16 + 8, y1 * 16 + 8), fill=(255, 44, 168))
        elif kind == 2:
            spr16(x0 * 16, y0 * 16, names.get('SPR_CRATE', 0))
            d.line((x0 * 16 + 8, y0 * 16, x0 * 16 + 8, (y1 + 1) * 16), fill=(148, 148, 192))
        elif kind == 3:
            tx = x1 if speed else x0
            spr16(tx * 16, y0 * 16, names.get('SPR_TURRET', 0), fx=bool(speed), fy=bool(y1))
            d.line((x0 * 16, y0 * 16 + 8, (x1 + 1) * 16, y0 * 16 + 8), fill=(255, 44, 168))
    ar = [g.u8('stage', STAGE_ARENA + k) for k in range(8)]
    if ar[0]:
        d.rectangle((ar[6] * 16, ar[7] * 16, ar[6] * 16 + 31, ar[7] * 16 + 31), outline=(255, 44, 168))
        d.text((ar[6] * 16 + 2, ar[7] * 16 + 10), 'BOSS', fill=(255, 44, 168))
    sx, sy = g.u8('stage', STAGE_W + 1), g.u8('stage', STAGE_W + 2)
    spr16(sx * 16, sy * 16, names.get('SPR_MECH_IDLE', 0), pal=spal[0])
    # a screen-sized grid, to judge distances
    for x in range(0, w * 16, 160):
        d.line((x, 0, x, 3), fill=(80, 80, 120))
    img = img.resize((img.width * a.scale, img.height * a.scale), Image.NEAREST)
    out = a.out or os.path.join(ROOT, 'build', 'maps', ('sector%s.png' % a.b) if a.a == 's' else ('stage_%s_%s.png' % (a.a, a.b)))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    img.save(out)
    print(out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
