"""Tests for tools/gen_assets.py and the generated src/gb/assets.[ch] (stdlib unittest).

Run: python3 -m unittest discover -s tests -p 'test_assets.py'
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import gen_assets as G  # noqa: E402

GEN = os.path.join(ROOT, 'tools', 'gen_assets.py')
ASSETS = os.path.join(ROOT, 'assets')


def c_array(src, name):
    """Integer values of `const uint8_t|uint16_t name[...] = {...};` (numbers only)."""
    m = re.search(r'const\s+uint(?:8|16)_t\s+%s\s*(?:\[[^\]]*\]\s*)+=\s*\{(.*?)\};' % name,
                  src, re.S)
    if not m:
        raise AssertionError('array %s not found' % name)
    body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S).replace('{', ',').replace('}', ',')
    return [int(v, 0) for v in body.replace('\n', ' ').split(',') if v.strip()]


def enum_names(h, first):
    m = re.search(r'enum \{\s*%s\b.*?\};' % first, h, re.S)
    if not m:
        raise AssertionError('enum starting with %s not found' % first)
    return re.findall(r'^\s+([A-Z][A-Z0-9_]*)', m.group(0), re.M)


class TestGenAssets(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix='al_assets_')
        rc = G.main(['--assets', ASSETS, '--out-dir', cls.tmp])
        assert rc == 0, 'generator failed'
        with open(os.path.join(cls.tmp, 'assets.h')) as f:
            cls.h = f.read()
        with open(os.path.join(cls.tmp, 'assets.c')) as f:
            cls.c = f.read()
        cls.data = G.load_assets(ASSETS)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def define(self, name):
        m = re.search(r'^#define\s+%s\s+(\S+)' % name, self.h, re.M)
        self.assertIsNotNone(m, 'missing #define %s' % name)
        return m.group(1)

    # ---- the contract (docs/CONTRACTS.md section 3) ----------------------
    def test_layout_defines(self):
        self.assertEqual(self.define('TILE_FONT_BASE'), '0')
        self.assertEqual(self.define('TILE_UI_BASE'), '64')
        self.assertEqual(self.define('TILE_ART_BASE'), '80')
        self.assertIn('#define FONT_TILE(ch)', self.h)
        for k in ('NUM_ART_TILES', 'NUM_SPR_TILES', 'LOGO_W', 'LOGO_H', 'LOGO_NTILES',
                  'PAL_WALL', 'NUM_DECK_PALS', 'DMG_BGP', 'DMG_OBP0', 'DMG_OBP1'):
            self.define(k)
        self.assertEqual(int(self.define('DMG_BGP'), 0), 0x1B)
        self.assertIn('#include <stdint.h>', self.h)
        self.assertIn('#include "assets.h"', self.c)

    def test_enums(self):
        self.assertEqual(enum_names(self.h, 'MT_EMPTY'), G.MT_ORDER + ['NUM_MT'])
        self.assertEqual(enum_names(self.h, 'ANIM_PLASMA'), G.ANIM_ORDER + ['NUM_ANIM'])
        self.assertEqual(enum_names(self.h, 'WC_TL'), ['WC_TL', 'WC_TR', 'WC_BL', 'WC_BR'])
        self.assertEqual(enum_names(self.h, 'WV_FILL'),
                         ['WV_FILL', 'WV_HEDGE', 'WV_VEDGE', 'WV_OUTER', 'WV_INNER',
                          'WV_FILL2', 'WALL_VARIANTS'])
        ui = enum_names(self.h, 'UI_BOLT')
        for n in G.UI_REQUIRED:
            self.assertIn(n, ui)
        spr = enum_names(self.h, 'SPR_MECH_IDLE')
        self.assertEqual(spr[:len(G.SPR_REQUIRED)], G.SPR_REQUIRED)
        self.assertEqual(enum_names(self.h, 'BOSS_SENTINEL'),
                         ['BOSS_SENTINEL', 'BOSS_PISTON', 'BOSS_HIVE', 'NUM_BOSS_ART'])
        self.assertEqual(G.BOSS_ORDER, ['BOSS_SENTINEL', 'BOSS_PISTON', 'BOSS_HIVE'])
        for n in G.BG_PAL_REQUIRED + G.SPR_PAL_REQUIRED:
            self.define(n)

    def test_declarations(self):
        for decl in ('uint8_t font_tiles[64*16]', 'uint8_t ui_tiles[16*16]',
                     'uint8_t art_tiles[NUM_ART_TILES*16]',
                     'uint8_t wall_tile[4*WALL_VARIANTS]', 'uint8_t mt_tiles[NUM_MT*4]',
                     'uint8_t mt_cgb_pal[NUM_MT]', 'uint8_t anim_mt[NUM_ANIM]',
                     'uint8_t anim_frames[NUM_ANIM]', 'uint8_t anim_speed[NUM_ANIM]',
                     'uint8_t *const anim_data[NUM_ANIM]',
                     'uint8_t spr_tiles[NUM_SPR_TILES*16]',
                     'uint8_t logo_tiles[LOGO_NTILES*16]', 'uint8_t logo_map[LOGO_W*LOGO_H]',
                     'uint8_t logo_cgb_attr[LOGO_W*LOGO_H]', 'uint16_t bg_cgb_pal[8*4]',
                     'uint16_t spr_cgb_pal[8*4]',
                     'uint16_t deck_cgb_pal[NUM_DECK_PALS*4]',
                     'uint8_t boss_tiles[NUM_BOSS_ART][32*16]'):
            self.assertIn('extern const %s;' % decl, self.h)

    def test_banked(self):
        self.assertIn('#pragma bank 255', self.c)
        self.assertIn('BANKREF(assets)', self.c)
        i = self.c.index('#ifdef __SDCC')
        self.assertLess(i, self.c.index('#pragma bank 255'))

    # ---- counts and limits -------------------------------------------------
    def test_counts_and_sizes(self):
        n_art = int(self.define('NUM_ART_TILES'))
        n_spr = int(self.define('NUM_SPR_TILES'))
        self.assertLessEqual(n_art, 176)
        self.assertLessEqual(n_spr, 96)   # tiles 96..127 are boss art
        self.assertEqual(self.define('BOSS_TILE_BASE'), '96')
        self.assertEqual(len(c_array(self.c, 'boss_tiles')), len(G.BOSS_ORDER) * 32 * 16)
        sizes = {'font_tiles': 64 * 16, 'ui_tiles': 16 * 16, 'art_tiles': n_art * 16,
                 'spr_tiles': n_spr * 16, 'mt_tiles': len(G.MT_ORDER) * 4,
                 'mt_cgb_pal': len(G.MT_ORDER), 'wall_tile': 4 * len(G.WALL_VARIANTS),
                 'anim_frames': len(G.ANIM_ORDER),
                 'anim_speed': len(G.ANIM_ORDER), 'bg_cgb_pal': 32, 'spr_cgb_pal': 32}
        for name, n in sizes.items():
            vals = c_array(self.c, name)
            self.assertEqual(len(vals), n, name)
            if name not in ('bg_cgb_pal', 'spr_cgb_pal'):
                self.assertTrue(all(0 <= v <= 255 for v in vals), name)
        lw, lh, nt = (int(self.define(k)) for k in ('LOGO_W', 'LOGO_H', 'LOGO_NTILES'))
        self.assertLessEqual(lw, 20)
        self.assertTrue(5 <= lh <= 8)
        self.assertLessEqual(nt, 176)
        self.assertEqual(len(c_array(self.c, 'logo_tiles')), nt * 16)
        self.assertEqual(len(c_array(self.c, 'logo_map')), lw * lh)
        self.assertEqual(len(c_array(self.c, 'logo_cgb_attr')), lw * lh)
        nd = int(self.define('NUM_DECK_PALS'))
        self.assertGreaterEqual(nd, 4)
        self.assertEqual(len(c_array(self.c, 'deck_cgb_pal')), nd * 4)

    def test_indices_in_range(self):
        n_art = int(self.define('NUM_ART_TILES'))
        art_end = G.TILE_ART_BASE + n_art
        for v in c_array(self.c, 'mt_tiles') + c_array(self.c, 'wall_tile'):
            self.assertTrue(v == 0 or G.TILE_ART_BASE <= v < art_end, v)
        for v in c_array(self.c, 'wall_tile'):
            self.assertGreaterEqual(v, G.TILE_ART_BASE)   # walls are never blank
        nt = int(self.define('LOGO_NTILES'))
        for v in c_array(self.c, 'logo_map'):
            self.assertTrue(v < G.TILE_ART_BASE or v < G.TILE_ART_BASE + nt, v)
        for v in c_array(self.c, 'logo_cgb_attr') + c_array(self.c, 'mt_cgb_pal'):
            self.assertTrue(0 <= v <= 7)
        ui = [int(x) for x in re.findall(r'UI_\w+ = TILE_UI_BASE \+ (\d+)', self.h)]
        self.assertTrue(ui and max(ui) < 16)
        n_spr = int(self.define('NUM_SPR_TILES'))
        for name, val in re.findall(r'^\s+(SPR_\w+) = (\d+)', self.h, re.M):
            big = name.startswith(G.SPR_16X16_PREFIXES)
            self.assertLessEqual(int(val) + (4 if big else 1), n_spr, name)
        for f in c_array(self.c, 'anim_frames'):
            self.assertTrue(2 <= f <= 8, f)
        for s in c_array(self.c, 'anim_speed'):
            self.assertTrue(1 <= s <= 60, s)

    def test_space_blank_and_mt_empty_black(self):
        self.assertEqual(c_array(self.c, 'font_tiles')[:16], [0] * 16)
        self.assertEqual(c_array(self.c, 'mt_tiles')[:4], [0, 0, 0, 0])

    def test_anim_tiles_exclusive(self):
        """Animated metatiles own 4 distinct art tiles used nowhere else."""
        mt = c_array(self.c, 'mt_tiles')
        walls = c_array(self.c, 'wall_tile')
        # anim_mt is written with the MT_* enum names
        anim_mt = [G.MT_ORDER.index(n) for n in re.findall(r'(MT_\w+), /\* ANIM_', self.c)]
        self.assertEqual(len(anim_mt), len(G.ANIM_ORDER))
        for m in anim_mt:
            own = mt[m * 4:m * 4 + 4]
            self.assertEqual(len(set(own)), 4, G.MT_ORDER[m])
            self.assertTrue(all(v >= G.TILE_ART_BASE for v in own))
            others = walls + [v for i, v in enumerate(mt) if i // 4 != m]
            for v in own:
                self.assertNotIn(v, others, G.MT_ORDER[m])
        # the anim frame 0 equals the metatile's art
        art = c_array(self.c, 'art_tiles')
        for a, (name, m, speed, frames) in enumerate(self.data['anim']):
            for c in range(4):
                idx = mt[m * 4 + c] - G.TILE_ART_BASE
                self.assertEqual(art[idx * 16:idx * 16 + 16], G.encode_tile(frames[0][c]))

    def test_anim_data_layout(self):
        frames = c_array(self.c, 'anim_frames')
        data = c_array(self.c, 'anim_frame_data')
        self.assertEqual(len(data), sum(frames) * 64)
        offs = [int(x) for x in re.findall(r'anim_frame_data \+ (\d+)', self.c)]
        self.assertEqual(len(offs), len(frames))
        pos = 0
        for o, f in zip(offs, frames):
            self.assertEqual(o, pos)
            pos += f * 64

    def test_palettes(self):
        bg = c_array(self.c, 'bg_cgb_pal')
        deck = c_array(self.c, 'deck_cgb_pal')
        for v in bg + deck + c_array(self.c, 'spr_cgb_pal'):
            self.assertLess(v, 0x8000)
        for i in range(8):
            self.assertEqual(bg[i * 4], 0, 'BG palette %d colour 0 must be black' % i)
        for i in range(len(deck) // 4):
            self.assertEqual(deck[i * 4], 0)
        pw = int(self.define('PAL_WALL'))
        self.assertEqual(deck[:4], bg[pw * 4:pw * 4 + 4])

    def test_tiles_valid(self):
        d = self.data
        tiles = list(d['font']) + [t for _, t in d['ui']] + list(d['art']) + \
            [t for _, t in d['spr']] + list(d['logo_tiles']) + \
            [t for _, bt in d['boss'] for t in bt]
        for t in tiles:
            self.assertEqual(len(t), 8)
            for row in t:
                self.assertEqual(len(row), 8)
                self.assertTrue(all(v in (0, 1, 2, 3) for v in row))
        for fn in sorted(f for f in os.listdir(ASSETS) if f.endswith('.txt')):
            for b in G.parse_file(os.path.join(ASSETS, fn)):
                if b.kind in G.SINGLE_LINE or b.kind == 'logo_attr':
                    continue
                self.assertEqual(b.w % 8, 0, b.where)
                self.assertEqual(b.h % 8, 0, b.where)

    def test_art_deduped(self):
        keys = [tuple(G.encode_tile(t)) for t in self.data['art']]
        anim_tiles = set()
        mt = c_array(self.c, 'mt_tiles')
        for _, m, _, _ in self.data['anim']:
            anim_tiles.update(v - G.TILE_ART_BASE for v in mt[m * 4:m * 4 + 4])
        static = [k for i, k in enumerate(keys) if i not in anim_tiles]
        self.assertEqual(len(static), len(set(static)), 'duplicate static art tiles')
        self.assertNotIn(tuple([0] * 16), static, 'blank art tile: use tile 0')

    def test_encoding(self):
        t = [[0, 1, 2, 3, 0, 0, 0, 3]] + [[0] * 8] * 7
        e = G.encode_tile(t)
        self.assertEqual(e[0], 0b01010001)
        self.assertEqual(e[1], 0b00110001)

    def test_crate_sprite_matches_metatile(self):
        mt = c_array(self.c, 'mt_tiles')
        art = c_array(self.c, 'art_tiles')
        spr = c_array(self.c, 'spr_tiles')
        base = int(re.search(r'SPR_CRATE = (\d+)', self.h).group(1))
        m = G.MT_ORDER.index('MT_CRATE')
        for c in range(4):
            a = mt[m * 4 + c] - G.TILE_ART_BASE
            s = base + c
            self.assertEqual(art[a * 16:a * 16 + 16], spr[s * 16:s * 16 + 16])

    def test_removed_and_renamed_names(self):
        gone = ['SPR_ARROW_R', 'SPR_ARROW_U', 'SPR_FLIP_ICON', 'SPR_THRUST',
                'MT_PLATE_C', 'MT_PLATE_C_ON', 'MT_FIELD_V', 'MT_FIELD_H', 'MT_EXIT_U',
                'MT_EXIT_D', 'MT_HATCH_L', 'MT_HATCH_T', 'MT_HATCH_B', 'ANIM_FIELD_V',
                'ANIM_FIELD_H', 'ANIM_EXIT_U', 'ANIM_EXIT_D', 'UI_STEP', 'UI_STAR', 'UI_FLAG']
        for n in gone:
            self.assertIsNone(re.search(r'\b%s\b' % n, self.h), n)
        for n in ('UI_HULL', 'UI_HULL_EMPTY', 'UI_SHIELD', 'SPR_RING', 'SPR_SHOT',
                  'SPR_SHOT_POWER', 'SPR_BOLT', 'SPR_TURRET', 'SPR_TURRET_FIRE',
                  'SPR_EXPLODE1', 'SPR_EXPLODE2', 'SPR_MECH_SHOOT', 'MT_CRATE', 'MT_DOOR_OPEN',
                  'ANIM_DOOR', 'MT_ITEM_POWER', 'MT_ITEM_SHIELD', 'MT_ITEM_BOMB',
                  'MT_ITEM_REPAIR', 'MT_BOSS_DOOR', 'MT_DECO_PIPE', 'MT_DECO_PIPE_V',
                  'MT_DECO_WINDOW', 'MT_DECO_LIGHT', 'MT_DECO_GRATE'):
            self.assertRegex(self.h, r'\b%s\b' % n)

    def test_boss_art(self):
        boss = dict(self.data['boss'])
        self.assertEqual([n for n, _ in self.data['boss']], G.BOSS_ORDER)
        flat = c_array(self.c, 'boss_tiles')
        for b, name in enumerate(G.BOSS_ORDER):
            tiles = boss[name]
            self.assertEqual(len(tiles), 32)
            enc = [v for t in tiles for v in G.encode_tile(t)]
            self.assertEqual(flat[b * 512:(b + 1) * 512], enc, name)
            f0, f1 = enc[:256], enc[256:]
            self.assertNotEqual(f0, f1, '%s: the two frames must differ' % name)
            for f in (f0, f1):
                self.assertGreater(sum(1 for v in f if v), 64, '%s: frame nearly empty' % name)

    def test_deco_is_background(self):
        """Decoration stays dark: colour 1 (+ at most a few colour-2 pixels)."""
        mts = dict((n, t) for n, _, _, t in self.data['mt'])
        for n in G.MT_ORDER:
            if not n.startswith('MT_DECO_'):
                continue
            px = [v for t in mts[n] for row in t for v in row]
            self.assertEqual(px.count(3), 0, n)
            self.assertLessEqual(px.count(2), 4, n)
            self.assertGreater(px.count(1), 16, n)

    def test_sprite_limit_enforced(self):
        tmp = tempfile.mkdtemp(prefix='al_assets_big_')
        try:
            for fn in os.listdir(ASSETS):
                if fn.endswith('.txt'):
                    shutil.copy(os.path.join(ASSETS, fn), tmp)
            n = G.BOSS_TILE_BASE - len(self.data['spr']) + 1
            with open(os.path.join(tmp, 'sprites.txt'), 'a') as f:
                for i in range(n):
                    f.write('\n@tile SPR_PAD_%d\n' % i + '\n'.join(['33333333'] * 8) + '\n')
            with self.assertRaises(G.AssetError):
                G.load_assets(tmp)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    # ---- generator behaviour ------------------------------------------------
    def test_deterministic(self):
        tmp2 = tempfile.mkdtemp(prefix='al_assets2_')
        try:
            G.main(['--assets', ASSETS, '--out-dir', tmp2])
            for fn, ref in (('assets.h', self.h), ('assets.c', self.c)):
                with open(os.path.join(tmp2, fn)) as f:
                    self.assertEqual(f.read(), ref)
        finally:
            shutil.rmtree(tmp2, ignore_errors=True)

    def test_check_mode(self):
        r = subprocess.run([sys.executable, GEN, '--check', '--out-dir', self.tmp],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(r.returncode, 0, r.stdout.decode())
        tmp2 = tempfile.mkdtemp(prefix='al_assets3_')
        try:
            shutil.copy(os.path.join(self.tmp, 'assets.h'), tmp2)
            with open(os.path.join(tmp2, 'assets.c'), 'w') as f:
                f.write(self.c + '/* stale */\n')
            r = subprocess.run([sys.executable, GEN, '--check', '--out-dir', tmp2],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(r.returncode, 0)
            with open(os.path.join(tmp2, 'assets.c')) as f:
                self.assertTrue(f.read().endswith('/* stale */\n'), '--check must not write')
        finally:
            shutil.rmtree(tmp2, ignore_errors=True)

    def test_committed_output_up_to_date(self):
        r = subprocess.run([sys.executable, GEN, '--check'],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(r.returncode, 0, 'run python3 tools/gen_assets.py\n' + r.stdout.decode())

    def test_gcc_compiles(self):
        gcc = shutil.which('gcc')
        if not gcc:
            self.skipTest('gcc not installed')
        out = os.path.join(self.tmp, 'a.o')
        r = subprocess.run([gcc, '-std=c99', '-Wall', '-Wextra', '-Werror', '-c',
                            '-o', out, os.path.join(self.tmp, 'assets.c')],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(r.returncode, 0, r.stdout.decode())

    def test_lcc_compiles(self):
        lcc = os.path.join(os.environ.get('GBDK_HOME', '/opt/gbdk'), 'bin', 'lcc')
        if not os.path.exists(lcc):
            self.skipTest('GBDK not installed')
        out = os.path.join(self.tmp, 'gb.o')
        r = subprocess.run([lcc, '-c', '-o', out, os.path.join(self.tmp, 'assets.c')],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(r.returncode, 0, r.stdout.decode())


if __name__ == '__main__':
    unittest.main()
