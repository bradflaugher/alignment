"""Shared helpers for the PyBoy ROM tests."""
import io, os, re, subprocess
from pyboy import PyBoy

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.path.join(ROOT, 'build', 'alignment.gb')
SYM = os.path.join(ROOT, 'build', 'alignment.sym')
MSGEN = os.path.join(ROOT, 'build', 'msgen')
PS_CARD, PS_PLAY, PS_DEAD, PS_CLEAR, PS_PAUSE = range(5)


MGBA = os.path.join(ROOT, 'build', 'mgba_server')


class Game:
    def __init__(self, cgb, sram=None):
        ram = io.BytesIO(sram if sram is not None else bytes(32768))
        if os.environ.get('ALIGNMENT_EMULATOR') == 'mgba':
            # a second opinion (make test-mgba): the same tests on headless mGBA
            import sys
            sys.path.insert(0, os.path.join(ROOT, 'tools', 'mgba'))
            import mgba_pyboy
            mgba_pyboy.SERVER = MGBA
            self.pb = mgba_pyboy.MgbaBoy(ROM, cgb=cgb, symbols=SYM, ram_file=ram)
        else:
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

    def start(self, mode, stage, seed=0):
        """Title -> a sector of the campaign (mode 1) or an endless stage (mode 2), via the
        game's test hook; returns when the stage is playable."""
        if self.pb.frame_count < 150:
            self.run(150 - self.pb.frame_count)
        self.poke('dbg_seed', 0, [seed & 0xFF, seed >> 8])
        self.poke('dbg_stage', 0, [stage & 0xFF, stage >> 8])
        self.poke('dbg_mode', 0, [mode])
        return self.wait(lambda: self.u8('play_state') == PS_PLAY, 3000)

    def start_sector(self, n):
        return self.start(1, n)

    def start_endless(self, seed, stage=1):
        return self.start(2, stage, seed)

    def mech_xy(self):
        return self.u16('player') >> 4, self.u16('player', 2) >> 4

    # ---- what the tests read of the game's state (offsets in its structs)
    def mech(self):
        """x px, y px, gravity, on the ground, air flip left, dead"""
        return (self.u16('player') >> 4, self.u16('player', 2) >> 4, self.u8('player', 8), self.u8('player', 9),
                self.u8('player', 10), self.u8('player', 13))

    def hull(self):
        return self.u8('player', 34)

    def arena(self):
        """kind, x0, x1, top, bot of the stage's boss arena"""
        a = STAGE_ARENA
        return (self.u8('stage', a), self.u8('stage', a + 2), self.u8('stage', a + 3), self.u8('stage', a + 4),
                self.u8('stage', a + 5))

    def boss(self):
        b = self.addr('boss')
        m = self.pb.memory
        return dict(kind=m[b], level=m[b + 1], active=m[b + 2], dead=m[b + 3], hp=m[b + 4], hpmax=m[b + 5],
                    state=m[b + 6], st=m[b + 7], x=m[b + 10] | m[b + 11] << 8, y=m[b + 12], open=m[b + 16])

    def keys(self, held):
        """hold exactly these buttons"""
        old = getattr(self, '_held', set())
        for b in old - set(held):
            self.pb.button_release(b)
        for b in set(held) - old:
            self.pb.button_press(b)
        self._held = set(held)

    def win_text(self, x, y, n):
        """n characters of the window's tile map at (x, y), as text"""
        t = self.pb.memory[0, 0x9C00 + y * 32 + x:0x9C00 + y * 32 + x + n]
        return ''.join(chr(c + 32) if c < 64 else '#' for c in t)

    def bkg_text(self, x, y, n):
        t = self.pb.memory[0, 0x9800 + y * 32 + x:0x9800 + y * 32 + x + n]
        return ''.join(chr(c + 32) if c < 64 else '#' for c in t)

    def hud_text(self):
        a = self.addr('hud_t')
        return ''.join(chr(c + 32) if c < 64 else '#' for c in self.pb.memory[a:a + 20])


STAGE_W = 2048              # offsets in Stage (src/core/stage.h)
STAGE_ARENA = 2256
RUN_STAGE, RUN_LIVES, RUN_DEATHS = 3, 5, 13     # offsets in Run


def asset_define(name):
    with open(os.path.join(ROOT, 'src', 'gb', 'assets.h')) as f:
        text = f.read()
    return int(re.search(r'#define\s+%s\s+(\d+)' % name, text).group(1))


def rom_symbol(name):
    """(bank, address) of a symbol, from the .sym file"""
    with open(SYM) as f:
        for line in f:
            p = line.split()
            if len(p) == 2 and p[1] == '_' + name:
                b, a = p[0].split(':')
                return int(b, 16), int(a, 16)
    raise KeyError(name)


def rom_data(name, n):
    """n bytes of a ROM table"""
    bank, addr = rom_symbol(name)
    with open(ROM, 'rb') as f:
        rom = f.read()
    off = addr if addr < 0x4000 else bank * 0x4000 + addr - 0x4000
    return rom[off:off + n]


def vram_tiles(g, first, n):
    """the pattern data of BG tiles first..first+n-1 (LCDC bit 4 clear: tile i < 128 at
    0x9000 + 16 i, the others at 0x8800 + 16 (i - 128))"""
    out = []
    for i in range(first, first + n):
        a = 0x9000 + i * 16 if i < 128 else 0x8800 + (i - 128) * 16
        out.append(bytes(g.pb.memory[0, a:a + 16]))
    return out


class VramWatch:
    """What is in video memory against what the game's own data says should be there.

    Real hardware drops a write to VRAM made while the LCD is drawing a line (mode 3);
    so does PyBoy. The game streams the map and its animations from the VBlank handler
    and patches cells from the main loop: if any of that ran late, a tile would stay
    wrong on the screen. look() is called once per frame of play and checks

    * the visible part of the background map (and, on a Color, its attributes) against
      the map the game prebuilt in cartridge RAM (banks 2 and 3), which its own patches
      (cells taken, checkpoint, arena doors) keep up to date;
    * the HUD row of the window against the row the game composed;
    * every art tile against the art in the ROM (a torn tile is half one, half another).

    A difference may last a few frames (a door column waits its turn in the VBlank
    queue, the HUD is refreshed an item a frame); one that stays is a failure."""

    MAP_FRAMES = 12
    HUD_FRAMES = 12
    ART_EVERY = 4

    def __init__(self, g):
        self.g = g
        self.cgb = g.tag == 'cgb'
        self.bad = []
        self.map_run = self.hud_run = self.art_run = 0
        self.looks = 0
        bank, _ = rom_symbol('art_tiles')
        with open(ROM, 'rb') as f:
            rom = f.read()
        data = rom[bank * 0x4000:(bank + 1) * 0x4000]
        self.art = set(data[i:i + 16] for i in range(len(data) - 15))
        self.first = asset_define('TILE_ART_BASE')
        self.count = asset_define('NUM_ART_TILES')

    def map_diff(self):
        g, m = self.g, self.g.pb.memory
        c0 = g.u16('cam_x') >> 4
        w = g.u8('stage', STAGE_W)
        vram = [m[0, 0x9800:0x9C00]] + ([m[1, 0x9800:0x9C00]] if self.cgb else [])
        out = []
        for cx in range(c0, min(c0 + 11, w)):
            for bank, v in enumerate(vram):
                a = 0xA000 + cx * 64
                # (PyBoy refuses a slice that ends at the bank's last byte)
                want = m[2 + bank, a:a + 63] + [m[2 + bank, a + 63]]
                for half in (0, 1):
                    mc = ((cx * 2) | half) & 31
                    if v[mc::32] != want[half * 32:half * 32 + 32]:
                        out.append((cx, half, 'attributes' if bank else 'tiles'))
        return out

    def look(self):
        g, m = self.g, self.g.pb.memory
        self.looks += 1
        d = self.map_diff()
        self.map_run = self.map_run + 1 if d else 0
        if self.map_run == self.MAP_FRAMES:
            self.bad.append('frame %d: the map on screen differs from the stage at %s' % (g.pb.frame_count, d[:4]))
        a = g.addr('hud_t')
        hud = m[0, 0x9C00:0x9C14] != m[a:a + 20]
        self.hud_run = self.hud_run + 1 if hud else 0
        if self.hud_run == self.HUD_FRAMES:
            self.bad.append('frame %d: the HUD row on screen is stale' % g.pb.frame_count)
        if self.looks % self.ART_EVERY == 0:
            torn = [self.first + i for i, t in enumerate(vram_tiles(g, self.first, self.count)) if t not in self.art]
            self.art_run = self.art_run + 1 if torn else 0
            if self.art_run == 2:
                self.bad.append('frame %d: art tiles %s are not any tile of the ROM (torn)' % (g.pb.frame_count, torn[:8]))
