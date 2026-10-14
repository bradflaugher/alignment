#!/usr/bin/env python3
"""Screenshots and a gameplay GIF for the README (PyBoy, headless).

    python3 tools/screenshots.py            # -> docs/screens/*.png, gameplay.gif
    python3 tools/screenshots.py --frames   # also dump every frame of the GIF run to build/frames/
"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tests'))
from romlib import Game, algen, ROOM_INDEX, ROOT
from PIL import Image

OUT = os.path.join(ROOT, 'docs', 'screens')
SEED = 0x1234


def grab(g):
    return g.pb.screen.image.convert('RGB').copy()


def main():
    os.makedirs(OUT, exist_ok=True)
    dump = '--frames' in sys.argv
    if dump:
        os.makedirs(os.path.join(ROOT, 'build', 'frames'), exist_ok=True)
    for cgb in (True, False):
        tag = 'cgb' if cgb else 'dmg'
        g = Game(cgb)
        g.run(150)
        grab(g).save(os.path.join(OUT, '%s_title.png' % tag))
        g.press('start', after=20)
        grab(g).save(os.path.join(OUT, '%s_menu.png' % tag))
        g.pb.button_press('b'); g.run(3); g.pb.button_release('b'); g.run(5)
        g.start_zen(SEED)
        frames = []
        btn = {'L': 'left', 'R': 'right', 'F': 'a'}
        for n in range(1, 9):
            ref = algen('dump', SEED, n)
            g.run(20)
            if n in (3, 5, 8):
                grab(g).save(os.path.join(OUT, '%s_room%d.png' % (tag, n)))
            for m in ref['solution']:
                g.wait(lambda: g.u8('play_idle') == 1, 3000)
                g.pb.button_press(btn[m])
                g.wait(lambda: g.u8('play_idle') == 0, 60)
                g.pb.button_release(btn[m])
                for _ in range(12):
                    g.run(2)
                    if cgb and n <= 5:
                        frames.append(grab(g))
            g.wait(lambda: g.u16('room', ROOM_INDEX) == n + 1 and g.u8('play_idle'), 20000)
        if cgb:
            frames = [f.resize((320, 288), Image.NEAREST) for f in frames]
            if dump:
                for i, f in enumerate(frames):
                    f.save(os.path.join(ROOT, 'build', 'frames', 'f%04d.png' % i))
            frames[0].save(os.path.join(OUT, 'gameplay.gif'), save_all=True, append_images=frames[1:],
                           duration=33, loop=0, optimize=True)
        print(tag, 'done')


if __name__ == '__main__':
    main()
