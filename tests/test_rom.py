"""End-to-end tests: boot the ROM in PyBoy (headless) on DMG and CGB and play it.

The host build of the core is the oracle. build/msgen's physics bot finds button
presses that finish a stage (boss included); replayed on the Game Boy, frame by frame,
they must finish it there too (same stage, same game code), without the mech being hit.
Frame budget: a Color (double speed) may miss a frame or two in the moments a boss
arena seals. An original Game Boy (half the CPU) slows down in the busiest stretches,
as the old games did: never for more than 20% of a stage's frames (a regression guard;
most sectors stay under 5%).

Video memory: hardware drops a write to VRAM made while the LCD draws a line, and so does
PyBoy, so every frame of play the tests compare what is in VRAM (the visible map, the HUD
row, the art tiles) with what the game's own data says should be there (romlib.VramWatch):
the bot's clean runs, and rough play with deaths, pauses and flips. The VBlank handler
also counts work that ended after VBlank (vbl_late), which must stay 0."""
import io
import random
import subprocess
import unittest
from romlib import (Game, MSGEN, PS_CARD, PS_PLAY, PS_DEAD, PS_CLEAR, PS_PAUSE, RUN_STAGE, RUN_LIVES,
                    RUN_DEATHS, VramWatch, asset_define, metatile, rom_data, vram_tiles)

SEED = 0x1234
BK_PISTON = 2
ARENA_ROWS = 8                      # src/core/stage.h


def piston_lock(level):             # PISTON_LOCK in src/core/world.h
    return 14 if level > 1 else 20


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
        watch = VramWatch(g)
        kind, x0, x1, top, bot = g.arena()
        fight, hung, slams = 0, 0, 0
        for _ in range(30000):
            if g.u8('play_state') != PS_PLAY:
                break
            g.run(1)
            watch.look()
            b = g.boss()
            if not b['active']:
                continue
            # the whole fight is on the screen: the camera holds the arena, floor to ceiling
            fight += 1
            if fight > 40:
                cam_y = g.u8('cam_y')
                self.assertEqual(cam_y, bot * 16 + 4 - 136, 'the camera left the arena on %s' % name)
                self.assertTrue(cam_y <= b['y'] <= cam_y + 136 - 16, 'the boss is off screen on %s' % name)
            # the piston hangs still (its warning) before every slam
            if b['kind'] == BK_PISTON and b['state'] == 1:
                if b['y'] == (top + 1) * 16:
                    hung += 1
                elif hung:
                    self.assertGreaterEqual(hung, piston_lock(b['level']), 'the piston slammed without warning on %s' % name)
                    hung, slams = 0, slams + 1
        else:
            self.fail('%s never ended' % name)
        self.assertEqual(watch.bad, [], 'video memory on %s (%s)' % (name, g.tag))
        self.assertEqual(g.u16('run', RUN_DEATHS), deaths0, 'the mech died on %s' % name)
        self.assertEqual(g.u8('play_state'), PS_CLEAR, '%s not cleared' % name)
        if kind:
            self.assertLessEqual(bot - top - 1, ARENA_ROWS, 'the arena of %s is taller than the screen' % name)
            self.assertTrue(fight, 'no boss fight on %s' % name)
        if kind == BK_PISTON:
            self.assertTrue(slams, 'the piston never slammed on %s' % name)
        lag, frames = g.u16('play_lag') - lag0, g.pb.frame_count - t0
        # the VBlank handler's own count of work that ended after VBlank
        self.assertEqual(g.u8('vbl_late'), 0, 'VBlank work ran past VBlank on %s' % name)
        if g.tag == 'cgb':
            self.assertLessEqual(lag, 4, 'missed frames on %s (CGB)' % name)
        else:
            self.assertLessEqual(lag, frames * 20 // 100, 'too much slowdown on %s (DMG): %d of %d frames' % (name, lag, frames))

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


def sram_image(save=None, fill=0):
    """a 32 KB cartridge RAM image, with `save` (bytes) as both copies of the save"""
    ram = bytearray([fill]) * 32768
    if save is not None:
        ram[0:len(save)] = save
        ram[0x20:0x20 + len(save)] = save
    return bytes(ram)


def save_bytes(sector=1, done=0, best_score=0, best_stage=0):
    """a valid save (src/gb/save.h), as the game writes it"""
    b = bytearray(b'AL' + bytes([3]))
    b += best_stage.to_bytes(2, 'little') + best_score.to_bytes(4, 'little')
    b += bytes([sector, done]) + bytes(4)
    c = 0x5A
    for v in b:
        c = ((c << 1 | c >> 7) & 0xFF) ^ v
    return bytes(b + bytes([c]))


def both(fn):
    """run a test on an original Game Boy and on a Color"""
    def run(self):
        for cgb in (False, True):
            with self.subTest('cgb' if cgb else 'dmg'):
                fn(self, cgb)
    run.__name__ = fn.__name__
    run.__doc__ = fn.__doc__
    return run


class Video(unittest.TestCase):
    """What reaches the screen, on the paths the bot's clean runs don't take."""

    def rough(self, cgb, sector, frames, seed):
        """a careless pilot: runs right most of the time, jumps, flips and fires at
        random, dies, comes back, pauses; never out of lives"""
        g = Game(cgb)
        g.run(150)
        self.assertTrue(g.start_sector(sector))
        rnd = random.Random(seed)
        watch = VramWatch(g)
        keys, deaths, pauses, flips = set(), 0, 0, 0
        for f in range(frames):
            state = g.u8('play_state')
            if state == PS_PAUSE:
                # (START or B resumes; never A: the cursor may be on QUIT)
                g.keys([])
                g.run(rnd.randint(2, 30))
                g.keys(['start'])
                g.run(2)
                g.keys([])
                g.run(3)
                keys = set()
                continue
            if rnd.random() < 0.08:
                keys = set(rnd.sample(['right', 'right', 'left', 'a', 'a', 'b', 'b', 'up', 'down', 'select'], rnd.randint(0, 3)))
                if rnd.random() < 0.7:
                    keys.add('right')
                    keys.discard('left')
                if rnd.random() < 0.02:
                    keys.add('start')
                    pauses += 1
            g.keys(keys)
            keys.discard('start')
            g.run(1)
            if g.u8('play_state') == PS_PLAY:
                watch.look()
            if g.u8('run', RUN_LIVES) < 2:
                g.poke('run', RUN_LIVES, [5])
            deaths = g.u16('run', RUN_DEATHS)
        g.keys([])
        self.assertEqual(watch.bad, [], 'video memory in rough play, sector %d (%s)' % (sector, g.tag))
        self.assertEqual(g.u8('vbl_late'), 0, 'VBlank work ran past VBlank')
        self.assertGreater(watch.looks, frames // 2)
        return deaths, pauses

    @both
    def test_rough_play(self, cgb):
        """deaths, respawns, pauses, flips and running back and forth: the map, the
        HUD and the art in video memory stay what the game means them to be"""
        deaths = pauses = 0
        for sector, seed in ((2, 7), (3, 3), (5, 11)):
            d, p = self.rough(cgb, sector, 4000, seed)
            deaths += d
            pauses += p
        # (the script is only worth something if it really dies and pauses)
        self.assertGreater(deaths, 3)
        self.assertGreater(pauses, 3)

    @both
    def test_screens_and_art(self, cgb):
        """the stage card, the pause box and the title are all there (text written from
        the main loop while the LCD is on), the art is whole after every load, and the
        LCD is never switched off after boot (on hardware that is a white flash)"""
        g = Game(cgb)
        off = []
        g.pb.hook_register(None, '_display_off', lambda c: c.append(g.pb.frame_count), off)
        g.run(300)
        boot_off = len(off)
        self.assertGreater(boot_off, 0, 'the display_off hook never fired (boot switches the LCD off once)')
        logo = rom_data('logo_tiles', asset_define('LOGO_NTILES') * 16)
        art = rom_data('art_tiles', asset_define('NUM_ART_TILES') * 16)
        base = asset_define('TILE_ART_BASE')

        def tiles_are(data, what):
            got = b''.join(vram_tiles(g, base, len(data) // 16))
            bad = [base + i for i in range(len(data) // 16) if got[i * 16:i * 16 + 16] != data[i * 16:i * 16 + 16]]
            self.assertEqual(bad, [], '%s tiles in video memory differ from the ROM' % what)

        tiles_are(logo, 'title logo')
        self.assertEqual(g.bkg_text(2, 8, 16), 'GRAVITY  STATION')
        for round_ in range(2):
            # title -> sector 2: the card
            g.poke('dbg_stage', 0, [2, 0])
            g.poke('dbg_mode', 0, [1])
            self.assertTrue(g.wait(lambda: g.win_text(5, 4, 6) == 'SECTOR', 600), 'no sector card')
            g.run(20)
            self.assertEqual(g.win_text(4, 6, 12), 'REACTOR CORE')
            self.assertEqual(g.win_text(6, 12, 7), 'WARNING')
            self.assertTrue(g.wait(lambda: g.u8('play_state') == PS_PLAY, 3000))
            # the art as loaded (the animated and gravity tiles hold other ROM tiles by now)
            watch = VramWatch(g)
            got = vram_tiles(g, base, len(art) // 16)
            same = sum(1 for i, t in enumerate(got) if t == art[i * 16:i * 16 + 16])
            self.assertGreater(same, len(got) * 3 // 4)
            self.assertTrue(all(t in watch.art for t in got), 'an art tile is torn after the load')
            # play a little, flip (the phase blocks' art is swapped), pause
            g.keys(['right'])
            g.run(40 + 7 * round_)
            g.keys(['select'])
            g.run(2)
            g.keys([])
            g.run(3 * round_)
            g.press('start', 2, 30)
            self.assertEqual(g.u8('play_state'), PS_PAUSE)
            self.assertEqual(g.win_text(7, 5, 6), 'PAUSED')
            self.assertEqual(g.win_text(5, 7, 7), '>RESUME')
            self.assertEqual(g.win_text(6, 9, 4), 'QUIT')
            # resume: the HUD is back, the map is whole
            g.press('start', 2, 2)
            self.assertEqual(g.u8('play_state'), PS_PLAY)
            for _ in range(40):
                g.run(1)
                watch.look()
            self.assertEqual(watch.bad, [])
            self.assertEqual(g.pb.memory[0, 0x9C00], g.u8('hud_t'))
            # pause again and quit: the title's logo is whole (no art copy landed on it)
            g.press('start', 2, 20)
            g.press('down', 2, 4)
            self.assertEqual(g.win_text(5, 9, 5), '>QUIT')
            g.press('a', 2, 2)
            self.assertTrue(g.wait(lambda: g.bkg_text(2, 8, 16) == 'GRAVITY  STATION', 600), 'no title after QUIT')
            g.run(60)
            tiles_are(logo, 'title logo (after a game)')
        self.assertEqual(len(off), boot_off, 'the LCD was switched off after boot (frames %s)' % off[boot_off:])
        self.assertEqual(g.u8('vbl_late'), 0)

    def test_dmg_flash_is_whole(self):
        """an original Game Boy's palette registers change in VBlank only: the flash of
        a gravity flip covers whole frames, never a band of one"""
        g = Game(False)
        g.run(150)
        self.assertTrue(g.start_sector(1))
        g.run(30)
        bgp = g.pb.memory[0xFF47]
        g.keys(['select'])
        shades = []
        for f in range(12):
            g.run(1)
            if f == 1:
                g.keys([])
            img = g.pb.screen.ndarray
            # the background shade of a line: its commonest pixel (sprites are small)
            rows = set()
            for y in range(20, 92, 4):
                line = [int(v) for v in img[y, :, 0]]
                rows.add(max(set(line), key=line.count))
            self.assertEqual(len(rows), 1, 'a band of the flash on frame %d: shades %s' % (f, sorted(rows)))
            shades.append(rows.pop())
        self.assertGreater(len(set(shades)), 1, 'no flash at all')
        self.assertEqual(shades[-1], shades[-2])
        self.assertEqual(g.pb.memory[0xFF47], bgp)

    @both
    def test_clear_screen(self, cgb):
        """the tally after a sector: its text is all there"""
        g = Game(cgb)
        g.run(150)
        load_demo(g, bot_path('s', 1))
        self.assertTrue(g.start_sector(1))
        self.assertTrue(g.wait(lambda: g.u8('play_state') == PS_CLEAR, 30000))
        self.assertTrue(g.wait(lambda: g.win_text(4, 11, 5) == 'SCORE', 300), 'no tally')
        self.assertEqual(g.win_text(4, 5, 12), 'SECTOR CLEAR')
        self.assertEqual(g.win_text(4, 9, 5), 'BONUS')
        self.assertEqual(g.win_text(8, 7, 1), '/')


class Feel(unittest.TestCase):
    """Controls and feedback."""

    def landing(self, g):
        """jump on the spot; returns the frames from the press to the landing"""
        g.keys(['a'])
        g.run(1)
        n = 1
        while g.mech()[3]:
            g.run(1)
            n += 1
            self.assertLess(n, 10)
        while not g.mech()[3]:
            g.run(1)
            n += 1
            self.assertLess(n, 120)
        return n

    @both
    def test_jump_buffer(self, cgb):
        """A pressed a few frames before landing jumps on landing; pressed long before,
        it doesn't (and a held A never bounces)"""
        def game():
            g = Game(cgb)
            g.run(150)
            self.assertTrue(g.start_sector(1))
            g.run(30)
            return g
        g = game()
        air = self.landing(g)
        g.keys([])
        self.assertGreater(air, 20)
        g.run(30)
        self.assertTrue(g.mech()[3])
        for early, jumps in ((3, True), (5, True), (12, False)):
            g = game()
            g.keys(['a'])
            g.run(air - early)
            self.assertFalse(g.mech()[3], 'landed sooner than measured')
            g.keys([])
            g.run(1)
            g.keys(['a'])       # too early: still in the air
            g.run(early + 6)
            self.assertEqual(not g.mech()[3], jumps, 'A pressed %d frames before landing' % early)
            g.keys([])
        # holding A through the landing is not a second jump
        g = game()
        g.keys(['a'])
        g.run(air + 8)
        self.assertTrue(g.mech()[3])
        g.keys([])

    @both
    def test_flip_buffer(self, cgb):
        """a flip pressed just before landing (the air flip already spent) happens on
        landing; pressed long before, it is refused as ever"""
        def game():
            # jump, flip in the air: the mech falls to the ceiling, its air flip spent
            g = Game(cgb)
            g.run(150)
            self.assertTrue(g.start_sector(1))
            g.run(30)
            g.press('a', 2, 6)
            g.press('select', 2, 0)
            self.assertEqual(g.mech()[2], 1)
            self.assertFalse(g.mech()[4], 'the air flip should be spent')
            return g
        g = game()
        fall = 0
        while not g.mech()[3]:
            g.run(1)
            fall += 1
            self.assertLess(fall, 200)
        self.assertGreater(fall, 25)
        for early, flips in ((3, True), (5, True), (15, False)):
            g = game()
            g.run(fall - early)
            self.assertFalse(g.mech()[3], 'landed sooner than measured')
            g.press('select', 2, early + 6)
            self.assertEqual(g.mech()[2], 0 if flips else 1, 'a flip pressed %d frames before landing' % early)

    @both
    def test_armour_feedback(self, cgb):
        """shots that bounce off a shut boss say so"""
        g = Game(cgb)
        g.run(150)
        load_demo(g, bot_path('s', 2))
        self.assertTrue(g.start_sector(2))
        self.assertTrue(g.wait(lambda: g.boss()['active'], 30000))
        self.assertTrue(g.wait(lambda: g.u8('ev_armour'), 600), 'no shot ever bounced off the piston')
        self.assertTrue(g.wait(lambda: 'ARMOUR SHUT' in g.hud_text(), 120), 'the HUD never said why')

    @both
    def test_laser_gates_show_before_they_burn(self, cgb):
        """a gate's lit art is on the screen by the frame it turns deadly (its art is
        copied in VBlank, a few frames after it is asked for), and gone while it is safe"""
        g = Game(cgb)
        g.run(150)
        load_demo(g, bot_path('s', 3))
        self.assertTrue(g.start_sector(3))
        base = asset_define('TILE_ART_BASE')
        art = rom_data('art_tiles', asset_define('NUM_ART_TILES') * 16)
        slot = g.u8('r_mt_tiles', metatile('MT_DOOR_CLOSED') * 4)
        dark = g.u8('r_mt_tiles', metatile('MT_DOOR_OPEN') * 4)
        off = art[(dark - base) * 16:(dark - base) * 16 + 64]
        lit = unlit = 0
        while g.u8('play_state') == PS_PLAY:
            g.run(1)
            t = g.u16('stage_t')
            shown = b''.join(vram_tiles(g, slot, 4))
            if (t & 127) < 72 and t > 8:            # (laser_on: 72 frames of every 128)
                self.assertNotEqual(shown, off, 'a laser gate burns unseen at stage time %d' % t)
                lit += 1
            elif 84 <= (t & 127) < 116:
                self.assertEqual(shown, off, 'a laser gate looks lit while it is safe, at stage time %d' % t)
                unlit += 1
        self.assertGreater(lit, 500)
        self.assertGreater(unlit, 200)

    @both
    def test_soft_reset(self, cgb):
        """A + B + SELECT + START goes back to the title from play, and the game still
        knows what it runs on"""
        g = Game(cgb)
        g.run(150)
        self.assertTrue(g.start_sector(1))
        g.run(40)
        g.keys(['a', 'b', 'select', 'start'])
        g.run(40)
        g.keys([])
        self.assertTrue(g.wait(lambda: g.bkg_text(2, 8, 16) == 'GRAVITY  STATION' and g.u8('stage', 2048) == 0, 900),
                        'no title after the reset')
        self.assertEqual(g.u8('is_cgb'), 1 if cgb else 0)
        g.run(60)
        self.assertEqual(g.bkg_text(5, 12, 11).strip() in ('PRESS START', ''), True)
        # and it plays again
        self.assertTrue(g.start_sector(1))


class Save(unittest.TestCase):
    """Battery RAM: fresh, garbage, a torn write."""

    def boot(self, sram):
        g = Game(False, sram)
        g.run(300)
        return g

    def saved(self, g):
        a = g.addr('save')
        return bytes(g.pb.memory[a:a + 16])

    def test_fresh_and_garbage(self):
        for fill in (0x00, 0xFF, 0xA5):
            g = self.boot(sram_image(fill=fill))
            self.assertEqual(self.saved(g), save_bytes(), 'cartridge RAM full of %02X' % fill)
            # (and both copies were written to the cartridge)
            self.assertEqual(bytes(g.pb.memory[0, 0xA000:0xA010]), save_bytes())
            self.assertEqual(bytes(g.pb.memory[0, 0xA020:0xA030]), save_bytes())

    def test_loads(self):
        s = save_bytes(sector=6, best_score=123456, best_stage=9)
        g = self.boot(sram_image(s))
        self.assertEqual(self.saved(g), s)
        self.assertEqual(g.bkg_text(4, 16, 7), '0123456')

    def test_torn_primary(self):
        s = save_bytes(sector=4, best_score=5000)
        ram = bytearray(sram_image(s))
        ram[5] ^= 0x40                      # (a write cut short: the checksum no longer fits)
        g = self.boot(bytes(ram))
        self.assertEqual(self.saved(g), s, 'the backup copy was not used')
        self.assertEqual(bytes(g.pb.memory[0, 0xA000:0xA010]), s, 'the primary copy was not repaired')

    def test_impossible_values(self):
        """a save that checks out but says sector 0 or 200 never starts a missing sector"""
        for sector in (0, 200):
            g = self.boot(sram_image(save_bytes(sector=sector)))
            g.press('start', 3, 20)
            g.press('a', 3, 3)
            self.assertTrue(g.wait(lambda: g.u8('play_state') == PS_PLAY, 3000), 'sector %d in the save' % sector)
            self.assertTrue(1 <= g.u16('run', RUN_STAGE) <= 8)
            self.assertGreater(g.u8('stage', 2048), 0)


if __name__ == '__main__':
    unittest.main()
