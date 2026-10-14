"""End-to-end tests: boot the ROM in PyBoy (headless) on DMG and CGB and play it.

The host build of the core is the oracle. build/msgen's physics bot finds button
presses that finish a stage; replayed on the Game Boy, frame by frame, they must finish
it there too (same stage, same physics), without the game ever missing a frame."""
import subprocess
import unittest
from romlib import Game, MSGEN, PS_PLAY, PS_CLEAR

SEED = 0x1234
# the bot's 9 moves: (held buttons, pressed on the first of the 4 frames)
MOVES = [((), ()), (('left',), ()), (('right',), ()),
         (('b',), ()), (('left', 'b'), ()), (('right', 'b'), ()),
         ((), ('a',)), (('left',), ('a',)), (('right',), ('a',))]


def bot_path(stage):
    return subprocess.check_output([MSGEN, 'path', str(SEED), str(stage)], text=True).strip()


def load_demo(g, path):
    """Hand the bot's moves to the game's demo input (they run on its stage clock)."""
    g.poke('demo_buf', 0, [int(c) for c in path])
    g.poke('demo_len', 0, [len(path) & 0xFF, len(path) >> 8])
    g.poke('demo_on', 0, [1])


class Stages(unittest.TestCase):
    def check(self, cgb, stages):
        g = Game(cgb)
        g.run(150)                       # (past PyBoy's boot logo and the game's own init)
        load_demo(g, bot_path(1))
        self.assertTrue(g.start_zen(SEED, 1), 'never reached the first stage')
        g.shot('stage1')
        for n in range(1, stages + 1):
            self.assertEqual(g.u16('run', 3), n)            # Run.stage
            lag0, deaths0 = g.u16('play_lag'), g.u16('run', 13)
            self.assertTrue(g.wait(lambda: g.u8('play_state') != PS_PLAY, 20000), 'stage %d never ended' % n)
            self.assertEqual(g.u16('run', 13), deaths0, 'the mech died on stage %d' % n)
            self.assertEqual(g.u8('play_state'), PS_CLEAR, 'stage %d not cleared' % n)
            self.assertEqual(g.u16('play_lag') - lag0, 0, 'missed frames on stage %d' % n)
            load_demo(g, bot_path(n + 1))
            self.assertTrue(g.wait(lambda: g.u16('run', 3) == n + 1 and g.u8('play_state') == PS_PLAY, 3000))
        g.shot('stage%d' % (stages + 1))

    def test_dmg(self):
        self.check(False, 3)

    def test_cgb(self):
        self.check(True, 3)


if __name__ == '__main__':
    unittest.main()
