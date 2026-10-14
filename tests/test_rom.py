"""End-to-end tests: boot the ROM in PyBoy (headless) on DMG and CGB and play it.

The host build of the core is the oracle. build/msgen's physics bot finds button
presses that finish a stage (boss included); replayed on the Game Boy, frame by frame,
they must finish it there too (same stage, same game code), without the mech being hit
and without the game ever missing a frame."""
import subprocess
import unittest
from romlib import Game, MSGEN, PS_PLAY, PS_CLEAR

SEED = 0x1234
RUN_STAGE, RUN_DEATHS = 3, 13       # offsets in Run


def bot_path(*stage):
    return subprocess.check_output([MSGEN, 'path'] + [str(s) for s in stage], text=True).strip()


def sectors():
    out = subprocess.check_output([MSGEN, 'show', 's', '1'], text=True)
    n = 1
    while subprocess.run([MSGEN, 'show', 's', str(n + 1)], capture_output=True).returncode == 0:
        n += 1
    return n


def load_demo(g, path):
    """Hand the bot's moves to the game's demo input (they run on its stage clock)."""
    assert len(path) <= 1024, 'bot path too long for the demo buffer'
    g.poke('demo_buf', 0, [int(c) for c in path])
    g.poke('demo_len', 0, [len(path) & 0xFF, len(path) >> 8])
    g.poke('demo_on', 0, [1])


class Play(unittest.TestCase):
    def clear_stage(self, g, name):
        lag0, deaths0 = g.u16('play_lag'), g.u16('run', RUN_DEATHS)
        self.assertTrue(g.wait(lambda: g.u8('play_state') != PS_PLAY, 30000), '%s never ended' % name)
        self.assertEqual(g.u16('run', RUN_DEATHS), deaths0, 'the mech died on %s' % name)
        self.assertEqual(g.u8('play_state'), PS_CLEAR, '%s not cleared' % name)
        self.assertEqual(g.u16('play_lag') - lag0, 0, 'missed frames on %s' % name)

    def campaign(self, cgb):
        for n in range(1, sectors() + 1):
            g = Game(cgb)
            g.run(150)                       # (past PyBoy's boot logo and the game's own init)
            load_demo(g, bot_path('s', n))
            self.assertTrue(g.start_sector(n), 'never reached sector %d' % n)
            self.assertEqual(g.u16('run', RUN_STAGE), n)
            self.clear_stage(g, 'sector %d' % n)
            g.shot('sector%d_clear' % n)

    def endless(self, cgb, stages):
        g = Game(cgb)
        g.run(150)
        load_demo(g, bot_path(SEED, 1))
        self.assertTrue(g.start_endless(SEED, 1), 'never reached the first stage')
        for n in range(1, stages + 1):
            self.assertEqual(g.u16('run', RUN_STAGE), n)
            self.clear_stage(g, 'endless stage %d' % n)
            load_demo(g, bot_path(SEED, n + 1))
            self.assertTrue(g.wait(lambda: g.u16('run', RUN_STAGE) == n + 1 and g.u8('play_state') == PS_PLAY, 3000))

    def test_campaign_dmg(self):
        self.campaign(False)

    def test_campaign_cgb(self):
        self.campaign(True)

    def test_endless_dmg(self):
        self.endless(False, 4)       # (stage 4 ends in a boss arena)

    def test_endless_cgb(self):
        self.endless(True, 4)


if __name__ == '__main__':
    unittest.main()
