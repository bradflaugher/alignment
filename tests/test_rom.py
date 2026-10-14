"""End-to-end tests: boot the ROM in PyBoy (headless) on DMG and CGB and play it.

The host build of the core is the oracle. build/msgen's physics bot finds button
presses that finish a stage (boss included); replayed on the Game Boy, frame by frame,
they must finish it there too (same stage, same game code), without the mech being hit.
Frame budget: a Color (double speed) may miss a frame or two in the moments a boss
arena seals; an original Game Boy may slow down briefly in heavy moments (as the old
games did), never for more than 6% of a stage."""
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
    assert len(path) <= 1536, 'bot path too long for the demo buffer'
    g.poke('demo_buf', 0, [int(c) for c in path])
    g.poke('demo_len', 0, [len(path) & 0xFF, len(path) >> 8])
    g.poke('demo_on', 0, [1])


class Play(unittest.TestCase):
    def clear_stage(self, g, name):
        lag0, deaths0, t0 = g.u16('play_lag'), g.u16('run', RUN_DEATHS), g.pb.frame_count
        self.assertTrue(g.wait(lambda: g.u8('play_state') != PS_PLAY, 30000), '%s never ended' % name)
        self.assertEqual(g.u16('run', RUN_DEATHS), deaths0, 'the mech died on %s' % name)
        self.assertEqual(g.u8('play_state'), PS_CLEAR, '%s not cleared' % name)
        lag, frames = g.u16('play_lag') - lag0, g.pb.frame_count - t0
        if g.tag == 'cgb':
            self.assertLessEqual(lag, 4, 'missed frames on %s (CGB)' % name)
        else:
            self.assertLessEqual(lag, frames * 6 // 100, 'too much slowdown on %s (DMG): %d of %d frames' % (name, lag, frames))

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
