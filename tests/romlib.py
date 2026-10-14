"""Shared helpers for the PyBoy ROM tests."""
import io, os, subprocess
from pyboy import PyBoy

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.path.join(ROOT, 'build', 'alignment.gb')
SYM = os.path.join(ROOT, 'build', 'alignment.sym')
ALGEN = os.path.join(ROOT, 'build', 'algen')
RN = 90
ROOM_PAR = 90 + 10      # offsetof(Room, par): cell[90] entry entry_side exit exit_side g0 npicks crate0[2] bot0[2] par
ROOM_INDEX = 90 + 15
SAVE_ZEN_SEED, SAVE_ZEN_DOOR = 7, 9


def algen(*args):
    out = subprocess.check_output([ALGEN] + [str(a) for a in args], text=True).split('\n')
    nums = [int(x) for x in out[0].split()]
    return {'par': nums[0], 'exit': nums[1], 'g0': nums[2], 'entry': nums[3], 'npicks': nums[4],
            'flips': nums[5], 'cells': nums[6:6 + RN], 'objs': nums[6 + RN:], 'solution': out[1].strip()}


class Game:
    def __init__(self, cgb, sram=None):
        ram = io.BytesIO(sram if sram is not None else bytes(32768))
        self.pb = PyBoy(ROM, window='null', cgb=cgb, symbols=SYM, sound_emulated=False, ram_file=ram)
        self.pb.set_emulation_speed(0)
        self.tag = 'cgb' if cgb else 'dmg'

    def addr(self, name):
        return self.pb.symbol_lookup('_' + name)[1]

    def u8(self, name, off=0):
        return self.pb.memory[self.addr(name) + off]

    def u16(self, name, off=0):
        a = self.addr(name) + off
        return self.pb.memory[a] | self.pb.memory[a + 1] << 8

    def poke(self, name, off, data):
        a = self.addr(name) + off
        for i, b in enumerate(data):
            self.pb.memory[a + i] = b

    def run(self, n=1):
        for _ in range(n):
            self.pb.tick()

    def press(self, button, hold=2, after=2):
        self.pb.button_press(button)
        self.run(hold)
        self.pb.button_release(button)
        self.run(after)

    def wait(self, cond, limit=3000):
        for _ in range(limit):
            if cond():
                return True
            self.pb.tick()
        return False

    def shot(self, name):
        d = os.path.join(ROOT, 'build', 'screens')
        os.makedirs(d, exist_ok=True)
        self.pb.screen.image.save(os.path.join(d, '%s_%s.png' % (name, self.tag)))

    def room_cells(self):
        a = self.addr('room')
        return [self.pb.memory[a + i] for i in range(RN)]

    def start_zen(self, seed):
        """Title -> ZEN WALK on a known seed, from room 1."""
        self.run(120)
        # first door: index 1, left side, cell (0,7), map (0,0)
        self.poke('save', SAVE_ZEN_SEED, [seed & 0xFF, seed >> 8])
        self.poke('save', SAVE_ZEN_DOOR, [1, 0, 0, 70, 0, 0, 0, 0])
        self.press('start', after=10)
        self.press('down')
        self.press('a')
        return self.wait(lambda: self.u8('play_idle') == 1, 6000)

    def play_moves(self, moves):
        btn = {'L': 'left', 'R': 'right', 'F': 'a'}
        for m in moves:
            if not self.wait(lambda: self.u8('play_idle') == 1, 3000):
                return False
            # hold until the game takes the move (it reads the pad once a frame)
            self.pb.button_press(btn[m])
            ok = self.wait(lambda: self.u8('play_idle') == 0, 60)
            self.pb.button_release(btn[m])
            self.run(1)
            if not ok:
                return False
        return True
