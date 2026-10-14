#!/usr/bin/env python3
"""Statistical profiler: sample the CPU's program counter once per frame while the
ROM plays (PyBoy, headless), and report where the time goes, by function.

    python3 tools/profile_rom.py [dmg|cgb] [frames]
"""
import bisect, collections, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tests'))
from romlib import Game, ROOT

cgb = len(sys.argv) > 1 and sys.argv[1] == 'cgb'
frames = int(sys.argv[2]) if len(sys.argv) > 2 else 1200
g = Game(cgb)
syms = collections.defaultdict(list)
for line in open(os.path.join(ROOT, 'build', 'alignment.sym')):
    p = line.split()
    if len(p) == 2 and ':' in p[0] and p[1].startswith('_'):
        b, a = p[0].split(':')
        syms[int(b, 16)].append((int(a, 16), p[1]))
for b in syms:
    syms[b].sort()
CB = g.addr('_current_bank') if False else 0xFF90
g.start_zen(0x1234)
cnt = collections.Counter()
btn = ['right', 'right', 'a', 'right', 'b']
for i in range(frames):
    if i % 40 == 0:
        g.pb.button_release(btn[(i // 40 - 1) % len(btn)])
        g.pb.button_press(btn[(i // 40) % len(btn)])
    g.pb.tick()
    pc = g.pb.register_file.PC
    bank = g.pb.memory[CB] if pc >= 0x4000 else 0
    lst = syms.get(bank if pc >= 0x4000 else 0, [])
    keys = [a for a, _ in lst]
    j = bisect.bisect_right(keys, pc) - 1
    cnt[lst[j][1] if j >= 0 else hex(pc)] += 1
tot = sum(cnt.values())
for n, c in cnt.most_common(25):
    print('%6.2f%% %s' % (100.0 * c / tot, n))
