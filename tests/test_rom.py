"""End-to-end tests: boot the ROM in PyBoy (headless) on DMG and CGB and play it.
The host build of the core (build/algen) is the oracle: the Game Boy must build the
same rooms byte for byte, and the host's optimal solutions must clear them on-device."""
import unittest
from romlib import Game, algen, ROOM_PAR, ROOM_INDEX

SEED = 0x1234


class PlayThrough(unittest.TestCase):
    def check_rooms(self, cgb, rooms):
        g = Game(cgb)
        self.assertTrue(g.start_zen(SEED), 'never reached the first room')
        g.shot('room1')
        for n in range(1, rooms + 1):
            ref = algen('dump', SEED, n)
            self.assertEqual(g.u16('room', ROOM_INDEX), n)
            self.assertEqual(g.room_cells(), ref['cells'], 'room %d differs' % n)
            self.assertEqual(g.u8('room', ROOM_PAR), ref['par'])
            self.assertTrue(g.play_moves(ref['solution']), 'stuck in room %d' % n)
            self.assertTrue(g.wait(lambda: g.u16('room', ROOM_INDEX) == n + 1 and g.u8('play_idle'), 20000),
                            'room %d not cleared by its solution %s' % (n, ref['solution']))
        g.shot('room%d' % (rooms + 1))

    def test_dmg(self):
        self.check_rooms(False, 6)

    def test_cgb(self):
        self.check_rooms(True, 6)


if __name__ == '__main__':
    unittest.main()
