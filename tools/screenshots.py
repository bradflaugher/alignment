#!/usr/bin/env python3
"""Screenshots and a gameplay GIF for the README (PyBoy, headless).

The physics bot's button presses for a sector are replayed by the game's demo input,
so the pictures show real play (the bot holds the cannon down all the time). The GIF
is played differently: the bot's search taps back and forth, which swings the camera,
so a scripted pilot plays the start of sector 1 the way a person would (see pilot()).

    python3 tools/screenshots.py            # -> docs/screens/*.png, gameplay.gif
"""
import os, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tests'))
from romlib import Game, MSGEN, ROOT, PS_PLAY
from PIL import Image

OUT = os.path.join(ROOT, 'docs', 'screens')


def grab(g):
    return g.pb.screen.image.convert('RGB').copy()


def demo(g, *stage):
    path = subprocess.check_output([MSGEN, 'path'] + [str(s) for s in stage], text=True).strip()
    g.poke('demo_buf', 0, [int(c) for c in path])
    g.poke('demo_len', 0, [len(path) & 0xFF, len(path) >> 8])
    g.poke('demo_on', 0, [1])
    return len(path) * 4


def boss_frame(g, limit):
    """Run until the boss fight has gone on a while; returns the frame count."""
    t = 0
    while g.u8('play_state') == PS_PLAY and t < limit:
        g.run(1)
        t += 1
        if g.u8('boss', 2) and g.u8('boss', 4) < g.u8('boss', 5) * 2 // 3:
            return True
    return False


# where the pilot turns gravity over in sector 1 (mech x, gravity it is in): over the
# tall block, then along the ceiling over the plasma
FLIPS = ((370, 0), (440, 1), (525, 0), (672, 1))
GIF_END_X = 850


def pilot(g):
    """Play the start of sector 1 like a person: run right, jump when blocked, shoot
    what is ahead, stop under a ceiling crawler and shoot it down with Up+B. Returns a
    frame for every frame of play."""
    held, frames, flips = set(), [], list(FLIPS)
    stuck, jump, flip, lastx = 0, 0, 0, None
    for _ in range(1800):
        x, y = g.mech_xy()
        grav, ground = g.u8('player', 8), g.u8('player', 9)
        if x >= GIF_END_X or g.u8('play_state') != PS_PLAY:
            break
        keys = {'right'}
        stuck = stuck + 1 if x == lastx else 0
        lastx = x
        if flips and x >= flips[0][0] and grav == flips[0][1]:
            flips.pop(0)
            flip = 3
        if flip:
            flip -= 1
            keys.add('select')
        elif jump:
            jump -= 1
            keys.add('a')
        elif ground and stuck >= 2:
            jump = 16
            keys.add('a')
        else:
            for k in range(g.u8('near_n')):
                ex, ey = g.u16('near', k * 11), g.u8('near', k * 11 + 2)
                above = (y - ey) if not grav else (ey - y)
                if -6 < ex - x < 4 and 8 < above < 120:
                    keys, stuck = {'up', 'b'}, -30
                    break
                if 0 < ex - x < 120 and abs(ey - y) < 12:
                    keys.add('b')
        for b in held - keys:
            g.pb.button_release(b)
        for b in keys - held:
            g.pb.button_press(b)
        held = keys
        g.run(1)
        frames.append(grab(g))
    for b in held:
        g.pb.button_release(b)
    if g.u8('player', 34) != 3 or x < GIF_END_X:
        print('warning: the GIF pilot was hit or stuck in sector 1 (at x=%d): check FLIPS' % x)
    return frames


def gameplay_gif(path):
    g = Game(True)
    g.run(150)
    g.start_sector(1)
    g.run(30)
    frames = pilot(g)[::2]
    # 3x, one palette for every frame (no colour flicker), 30 fps: 2 game frames each,
    # 33.3 ms as 30, 30, 40 ms (a GIF counts in hundredths of a second)
    frames = [f.resize((480, 432), Image.NEAREST) for f in frames]
    colours = sorted(set(c for f in frames[::10] for _, c in f.getcolors(4096)))
    sheet = Image.new('RGB', (len(colours), 1))
    sheet.putdata(colours)
    pal = sheet.quantize(len(colours))
    frames = [f.quantize(palette=pal, dither=Image.Dither.NONE) for f in frames]
    frames[0].save(path, save_all=True, append_images=frames[1:], loop=0, optimize=False,
                   duration=[30 + 10 * (i % 3 == 2) for i in range(len(frames))])


def sectors():
    n = 0
    while subprocess.run([MSGEN, 'show', 's', str(n + 1)], capture_output=True).returncode == 0:
        n += 1
    return n


def main():
    os.makedirs(OUT, exist_ok=True)
    for name in os.listdir(OUT):
        if name.endswith('.png') or name.endswith('.gif'):
            os.remove(os.path.join(OUT, name))
    n_sectors = sectors()
    for cgb in (True, False):
        tag = 'cgb' if cgb else 'dmg'
        g = Game(cgb)
        g.run(240)
        grab(g).save(os.path.join(OUT, '%s_title.png' % tag))
        for n in range(1, n_sectors + 1):
            g = Game(cgb)
            g.run(150)
            frames_total = demo(g, 's', n)
            g.start_sector(n)
            t = 0
            # two moments in the sector, then the boss
            for k, at in enumerate((frames_total // 6, frames_total // 2)):
                while g.u8('play_state') == PS_PLAY and t < at:
                    g.run(1)
                    t += 1
                grab(g).save(os.path.join(OUT, '%s_sector%d_%d.png' % (tag, n, k + 1)))
            if boss_frame(g, frames_total * 2):
                grab(g).save(os.path.join(OUT, '%s_sector%d_boss.png' % (tag, n)))
        print(tag, 'done')
    gameplay_gif(os.path.join(OUT, 'gameplay.gif'))
    # whole-sector maps
    for n in range(1, n_sectors + 1):
        subprocess.check_call([sys.executable, os.path.join(ROOT, 'tools', 'level_map.py'), 's', str(n),
                               '--scale', '1', '--out', os.path.join(OUT, 'map_sector%d.png' % n)])


if __name__ == '__main__':
    main()
