#!/usr/bin/env python3
"""ALIGNMENT level compiler: the hand-made sectors.

Reads ``assets/levels/sector*.txt`` (in name order) and writes
``src/core/level_data.h``, which ``src/core/levels.c`` includes and decodes with
``stage_load()``. Stdlib only, deterministic.

    python3 tools/gen_levels.py            # (re)write src/core/level_data.h
    python3 tools/gen_levels.py --check    # exit 1 if it is out of date
    python3 tools/gen_levels.py --dump N   # print sector N as the game will see it

Level file
----------
Header lines ``key: value`` then ``map:`` and exactly 16 rows of the map (row 0 at the
top). The widest row sets the width (at most 128); shorter rows are padded with wall.
Lines starting with ``;`` are comments (``#`` is wall).

    name: HANGAR BAY        (shown on the sector card, at most 16 characters)
    deck: 0                 (colours 0-7)
    speed: 1                (drones' speed 1-3)
    boss: SENTINEL 1        (SENTINEL / PISTON / HIVE / TWIN, level 1 or 2; omit for no boss)

Map characters
--------------
    #  wall                  .  empty (a space is empty too)
    ~  plasma (deadly)       L  laser gate (blinks)
    P  purple phase block: solid while gravity is DOWN
    G  green phase block: solid while gravity is UP
    o  energy cell           C  checkpoint (exactly one)    S  start (exactly one, needs a floor under it)
    E  exit gate (a column of them; the stage is cleared on touch)
    p  power shot   s  shield   b  gravity bomb   r  repair          (power-ups)
    =  girder   -  pipe   |  vertical pipe   O  window   *  light   %  grate   (background)
    Enemies (the cell itself is empty):
    c  crawler on the floor below it (patrols the flat floor, up to 4 cells either way)
    k  crawler on the ceiling above it
    v  hover drone (bobs from here down to the first solid cell)
    X  crusher (slams from here down to the floor)
    <  >  turret on the floor, firing left / right
    {  }  turret on the ceiling, firing left / right
    Boss arena:
    d  entry door cells (open; they seal behind the mech)
    D  exit door cells (shut until the boss is destroyed)
    B  the boss's top-left cell
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, 'assets', 'levels')
OUT = os.path.join(ROOT, 'src', 'core', 'level_data.h')

SH = 16
SW_MAX = 128
MAX_ENEMIES = 24

# terrain values: must match src/core/stage.h
T = dict(EMPTY=0, WALL=1, PLASMA=2, PHASE_P=3, PHASE_G=4, CELL=5, CHECK=6, EXIT=7, LASER=8,
         GIRDER=9, ITEM_P=10, ITEM_S=11, ITEM_B=12, ITEM_R=13, DOOR=14, DOOR_OPEN=15,
         DECO_PIPE=16, DECO_PIPE_V=17, DECO_WINDOW=18, DECO_LIGHT=19, DECO_GRATE=20)
E_CRAWLER, E_HOVER, E_CRUSHER, E_TURRET = range(4)
BOSSES = {'SENTINEL': 1, 'PISTON': 2, 'HIVE': 3, 'TWIN': 4}

CHARS = {
    '#': 'WALL', '.': 'EMPTY', ' ': 'EMPTY', '~': 'PLASMA', 'L': 'LASER',
    'P': 'PHASE_P', 'G': 'PHASE_G', 'o': 'CELL', 'C': 'CHECK', 'S': 'EMPTY', 'E': 'EXIT',
    'p': 'ITEM_P', 's': 'ITEM_S', 'b': 'ITEM_B', 'r': 'ITEM_R',
    '=': 'GIRDER', '-': 'DECO_PIPE', '|': 'DECO_PIPE_V', 'O': 'DECO_WINDOW', '*': 'DECO_LIGHT',
    '%': 'DECO_GRATE', 'd': 'DOOR_OPEN', 'D': 'DOOR', 'B': 'EMPTY',
    'c': 'EMPTY', 'k': 'EMPTY', 'v': 'EMPTY', 'X': 'EMPTY', '<': 'EMPTY', '>': 'EMPTY',
    '{': 'EMPTY', '}': 'EMPTY',
}


class LevelError(Exception):
    pass


def solid_floor(t):
    return t in (T['WALL'], T['DOOR'], T['PHASE_P'], T['PHASE_G'])


def walkable(t):
    return t in (T['EMPTY'], T['CELL'], T['GIRDER'], T['DECO_PIPE'], T['DECO_PIPE_V'],
                 T['DECO_WINDOW'], T['DECO_LIGHT'], T['DECO_GRATE'], T['ITEM_P'], T['ITEM_S'],
                 T['ITEM_B'], T['ITEM_R'], T['CHECK'])


def parse(path):
    head, rows, in_map = {}, [], False
    for n, line in enumerate(open(path, encoding='utf-8').read().split('\n'), 1):
        if not in_map:
            s = line.strip()
            if not s or s.startswith(';'):
                continue
            if s == 'map:':
                in_map = True
                continue
            if ':' not in s:
                raise LevelError('%s:%d: expected "key: value"' % (path, n))
            k, v = s.split(':', 1)
            head[k.strip()] = v.strip()
        else:
            if line.startswith(';'):
                continue
            if len(rows) < SH:
                rows.append(line.rstrip('\n'))
            elif line.strip():
                raise LevelError('%s:%d: more than %d map rows' % (path, n, SH))
    if len(rows) != SH:
        raise LevelError('%s: the map needs exactly %d rows (has %d)' % (path, SH, len(rows)))
    return head, rows


def compile_level(path):
    head, rows = parse(path)
    name = os.path.basename(path)
    w = max(len(r) for r in rows)
    if w > SW_MAX:
        raise LevelError('%s: %d columns (at most %d)' % (name, w, SW_MAX))
    if w < 20:
        raise LevelError('%s: too narrow' % name)
    grid = [r.ljust(w, '#') for r in rows]
    cell = [[0] * SH for _ in range(w)]
    marks = {}
    for y in range(SH):
        for x in range(w):
            ch = grid[y][x]
            if ch not in CHARS:
                raise LevelError('%s: unknown map character %r at column %d row %d' % (name, ch, x, y))
            cell[x][y] = T[CHARS[ch]]
            marks.setdefault(ch, []).append((x, y))
    for ch in 'SC':
        if len(marks.get(ch, [])) != 1:
            raise LevelError('%s: needs exactly one %r (has %d)' % (name, ch, len(marks.get(ch, []))))
    if not marks.get('E'):
        raise LevelError('%s: no exit gate E' % name)
    sx, sy = marks['S'][0]
    if sy + 1 >= SH or not solid_floor(cell[sx][sy + 1]):
        raise LevelError('%s: the start S needs a floor under it' % name)
    cx, cy = marks['C'][0]
    ex = min(x for x, _ in marks['E'])
    ey = max(y for x, y in marks['E'] if x == ex)

    def at(x, y):
        if x < 0 or x >= w or y < 0 or y >= SH:
            return T['WALL']
        return cell[x][y]

    speed = int(head.get('speed', '1'))
    if not 1 <= speed <= 3:
        raise LevelError('%s: speed must be 1-3' % name)
    enemies = []
    for ch, lst in sorted(marks.items()):
        for (x, y) in lst:
            phase = (x * 37 + y * 11) & 255
            if ch in 'ck':
                up = ch == 'k'
                def ok(xx):
                    return walkable(at(xx, y)) and solid_floor(at(xx, y - 1 if up else y + 1))
                if not ok(x):
                    raise LevelError('%s: crawler at %d,%d has no %s' % (name, x, y, 'ceiling' if up else 'floor'))
                x0 = x
                while ok(x0 - 1) and x - x0 < 4:
                    x0 -= 1
                x1 = x
                while ok(x1 + 1) and x1 - x < 4:
                    x1 += 1
                if x1 - x0 > 15:
                    raise LevelError('%s: crawler at %d,%d patrols %d cells (at most 16)' % (name, x, y, x1 - x0 + 1))
                enemies.append((E_CRAWLER, x0, x1, y, 1 if up else 0, speed, phase))
            elif ch == 'v':
                y1 = y
                while walkable(at(x, y1 + 1)):
                    y1 += 1
                if y1 == y:
                    raise LevelError('%s: hover drone at %d,%d has no room to move' % (name, x, y))
                enemies.append((E_HOVER, x, x, y, y1, speed, phase))
            elif ch == 'X':
                y1 = y
                while walkable(at(x, y1 + 1)):
                    y1 += 1
                enemies.append((E_CRUSHER, x, x, y, y1, speed, phase))
            elif ch in '<>{}':
                ceil = ch in '{}'
                left = ch in '<{'
                if not solid_floor(at(x, y - 1 if ceil else y + 1)):
                    raise LevelError('%s: turret at %d,%d is not mounted on a %s' % (name, x, y, 'ceiling' if ceil else 'floor'))
                d = -1 if left else 1
                k = 1
                while k < 15:
                    t = at(x + d * k, y)
                    if solid_floor(t):
                        break
                    k += 1
                reach = x + d * (k - 1)
                x0, x1 = (reach, x) if left else (x, reach)
                enemies.append((E_TURRET, x0, x1, y, 1 if ceil else 0, 1 if left else 0, phase))
    enemies.sort(key=lambda e: (e[1], e[3]))
    if len(enemies) > MAX_ENEMIES:
        raise LevelError('%s: %d enemies (at most %d)' % (name, len(enemies), MAX_ENEMIES))

    arena = [0] * 8 + [0, 0]
    boss = head.get('boss')
    if boss:
        parts = boss.split()
        if parts[0] not in BOSSES:
            raise LevelError('%s: unknown boss %r' % (name, parts[0]))
        level = int(parts[1]) if len(parts) > 1 else 1
        dl, dd, bb = marks.get('d', []), marks.get('D', []), marks.get('B', [])
        if not dl or not dd or len(bb) != 1:
            raise LevelError('%s: a boss needs entry door d cells, exit door D cells and one B' % name)
        x0s, x1s = {x for x, _ in dl}, {x for x, _ in dd}
        if len(x0s) != 1 or len(x1s) != 1:
            raise LevelError('%s: each arena door must be one column' % name)
        ax0, ax1 = x0s.pop(), x1s.pop()
        bx, by = bb[0]
        if not ax0 < bx < ax1:
            raise LevelError('%s: B must be between the doors' % name)
        top = by
        while not solid_floor(at(bx, top)):
            top -= 1
        bot = by
        while not solid_floor(at(bx, bot)):
            bot += 1
        for x in range(ax0 + 1, ax1):
            if not solid_floor(at(x, top)) or not solid_floor(at(x, bot)):
                raise LevelError('%s: the arena needs a solid ceiling (row %d) and floor (row %d) across' % (name, top, bot))
        if ax1 - ax0 - 1 < 8:
            raise LevelError('%s: the arena is too narrow (at least 8 columns inside)' % name)
        if any(ax0 < e[1] < ax1 or ax0 < e[2] < ax1 for e in enemies):
            raise LevelError('%s: no enemies inside the arena' % name)
        m0 = sum(1 << y for _, y in dl)
        m1 = sum(1 << y for _, y in dd)
        arena = [BOSSES[parts[0]], level, ax0, ax1, top, bot, bx, by, m0, m1]
    elif marks.get('d') or marks.get('D') or marks.get('B'):
        raise LevelError('%s: arena doors / B without a boss: line' % name)

    title = head.get('name', name).upper()
    if len(title) > 16:
        raise LevelError('%s: name longer than 16 characters' % name)
    deck = int(head.get('deck', '0')) & 7

    flat = [cell[x][y] for x in range(w) for y in range(SH)]
    rle = []
    i = 0
    while i < len(flat):
        j = i
        while j < len(flat) and flat[j] == flat[i] and j - i < 255:
            j += 1
        rle += [j - i, flat[i]]
        i = j
    data = [w, sx, sy, cx, cy, ex, ey, deck]
    data += arena[:8] + [arena[8] & 255, arena[8] >> 8, arena[9] & 255, arena[9] >> 8]
    data += [len(enemies)]
    for e in enemies:
        data += list(e)
    data += rle
    return dict(name=title, data=data, w=w, enemies=len(enemies),
                cells=sum(1 for t in flat if t == T['CELL']), boss=boss or '-')


def level_files():
    if not os.path.isdir(SRC):
        return []
    return sorted(os.path.join(SRC, f) for f in os.listdir(SRC) if f.startswith('sector') and f.endswith('.txt'))


def generate():
    levels = [compile_level(p) for p in level_files()]
    out = ['/* generated by tools/gen_levels.py from assets/levels/ - do not edit */',
           '#define NUM_SECTORS %d' % len(levels), '']
    offs, blob = [], []
    for lv in levels:
        offs.append(len(blob))
        blob += lv['data']
    out.append('static const uint8_t lv_data[%d] = {' % max(1, len(blob)))
    for i in range(0, len(blob), 20):
        out.append('    ' + ', '.join(str(b) for b in blob[i:i + 20]) + ',')
    if not blob:
        out.append('    0')
    out.append('};')
    out.append('static const uint16_t lv_off[%d] = { %s };' % (max(1, len(offs)), ', '.join(str(o) for o in offs) or '0'))
    out.append('')
    out.append('/* sector names, for the sector card */')
    out.append('static const char *const lv_name[%d] = {' % max(1, len(levels)))
    for lv in levels:
        out.append('    "%s",' % lv['name'])
    if not levels:
        out.append('    ""')
    out.append('};')
    return '\n'.join(out) + '\n', levels


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--dump', type=int)
    a = ap.parse_args(argv)
    try:
        text, levels = generate()
    except LevelError as e:
        print('gen_levels: %s' % e, file=sys.stderr)
        return 1
    if a.dump:
        lv = levels[a.dump - 1]
        print(lv)
        return 0
    if a.check:
        old = open(OUT).read() if os.path.exists(OUT) else ''
        if old != text:
            print('gen_levels: %s is out of date' % OUT, file=sys.stderr)
            return 1
        return 0
    with open(OUT, 'w') as f:
        f.write(text)
    total = sum(len(lv['data']) for lv in levels)
    for i, lv in enumerate(levels, 1):
        print('sector %d %-16s %3d cols, %2d enemies, %2d cells, boss %s, %d bytes' %
              (i, lv['name'], lv['w'], lv['enemies'], lv['cells'], lv['boss'], len(lv['data'])))
    print('gen_levels: %d sectors, %d bytes -> %s' % (len(levels), total, OUT))
    return 0


if __name__ == '__main__':
    sys.exit(main())
