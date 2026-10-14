#!/usr/bin/env python3
"""Screenshots and a gameplay GIF for the README (PyBoy, headless).

The physics bot's input path for a stage is replayed by the game's demo input, so the
pictures show real play.

    python3 tools/screenshots.py            # -> docs/screens/*.png, gameplay.gif
"""
import os, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tests'))
from romlib import Game, MSGEN, ROOT, PS_PLAY
from PIL import Image

OUT = os.path.join(ROOT, 'docs', 'screens')
SEED = 0x1234
SHOTS = {3: (300, 700), 9: (400, 900), 25: (350, 800)}   # stage: frames to grab at


def grab(g):
    return g.pb.screen.image.convert('RGB').copy()


def demo(g, stage):
    path = subprocess.check_output([MSGEN, 'path', str(SEED), str(stage)], text=True).strip()
    g.poke('demo_buf', 0, [int(c) for c in path])
    g.poke('demo_len', 0, [len(path) & 0xFF, len(path) >> 8])
    g.poke('demo_on', 0, [1])


def main():
    os.makedirs(OUT, exist_ok=True)
    for name in os.listdir(OUT):
        if name.endswith('.png') or name.endswith('.gif'):
            os.remove(os.path.join(OUT, name))
    for cgb in (True, False):
        tag = 'cgb' if cgb else 'dmg'
        g = Game(cgb)
        g.run(240)
        grab(g).save(os.path.join(OUT, '%s_title.png' % tag))
        for stage, at in SHOTS.items():
            g = Game(cgb)
            g.run(150)
            demo(g, stage)
            g.start_zen(SEED, stage)
            frames, t = [], 0
            while g.u8('play_state') == PS_PLAY and t <= max(at):
                g.run(1)
                t += 1
                if t in at:
                    grab(g).save(os.path.join(OUT, '%s_stage%d_%d.png' % (tag, stage, at.index(t) + 1)))
                if cgb and stage == 9 and t % 2 == 0 and t < 600:
                    frames.append(grab(g).resize((320, 288), Image.NEAREST))
            if frames:
                frames[0].save(os.path.join(OUT, 'gameplay.gif'), save_all=True, append_images=frames[1:],
                               duration=33, loop=0, optimize=True)
        print(tag, 'done')


if __name__ == '__main__':
    main()
