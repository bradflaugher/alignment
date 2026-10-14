#!/usr/bin/env python3
"""Screenshots and a gameplay GIF for the README (PyBoy, headless).

The physics bot's button presses for a sector are replayed by the game's demo input,
so the pictures show real play (the bot holds the cannon down all the time).

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
            frames, t = [], 0
            # two moments in the sector, then the boss
            for k, at in enumerate((frames_total // 6, frames_total // 2)):
                while g.u8('play_state') == PS_PLAY and t < at:
                    g.run(1)
                    t += 1
                    if cgb and n == 1 and t % 2 == 0 and t < 900:
                        frames.append(grab(g).resize((320, 288), Image.NEAREST))
                grab(g).save(os.path.join(OUT, '%s_sector%d_%d.png' % (tag, n, k + 1)))
            if boss_frame(g, frames_total * 2):
                grab(g).save(os.path.join(OUT, '%s_sector%d_boss.png' % (tag, n)))
            if frames:
                frames[0].save(os.path.join(OUT, 'gameplay.gif'), save_all=True, append_images=frames[1:],
                               duration=33, loop=0, optimize=True)
        print(tag, 'done')
    # whole-sector maps
    for n in range(1, n_sectors + 1):
        subprocess.check_call([sys.executable, os.path.join(ROOT, 'tools', 'level_map.py'), 's', str(n),
                               '--scale', '1', '--out', os.path.join(OUT, 'map_sector%d.png' % n)])


if __name__ == '__main__':
    main()
