"""Shared helpers for the PyBoy ROM tests."""
import io, os, subprocess
from pyboy import PyBoy

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.path.join(ROOT, 'build', 'alignment.gb')
SYM = os.path.join(ROOT, 'build', 'alignment.sym')
MSGEN = os.path.join(ROOT, 'build', 'msgen')
SAVE_ZEN_SEED, SAVE_ZEN_STAGE = 9, 11
PS_CARD, PS_PLAY, PS_DEAD, PS_CLEAR, PS_PAUSE = range(5)


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

    def press(self, button, hold=3, after=3):
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

    def start_zen(self, seed, stage=1):
        """Title -> ZEN on a known seed and stage; returns when the stage is playable."""
        if self.pb.frame_count < 150:
            self.run(150 - self.pb.frame_count)
        self.poke('save', SAVE_ZEN_SEED, [seed & 0xFF, seed >> 8])
        self.poke('save', SAVE_ZEN_STAGE, [stage & 0xFF, stage >> 8])
        self.press('start', after=10)
        self.press('down')
        self.press('a')
        return self.wait(lambda: self.u8('play_state') == PS_PLAY, 3000)

    def mech_xy(self):
        return self.u16('mech') >> 4, self.u16('mech', 2) >> 4
